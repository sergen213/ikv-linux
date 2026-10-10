#include "ikv/launcher_extraction.hpp"

#include <QCryptographicHash>
#include <QByteArrayView>
#include <QDir>
#include <QDirIterator>
#include <QDeadlineTimer>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QTemporaryDir>
#include <QUuid>
#include <array>
#include <cerrno>
#include <stdexcept>
#include <fcntl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>

namespace ikv::launcher_extraction {
namespace {
constexpr qint64 installerBytes = 1684201720;
constexpr quint64 payloadBytes = 4075408963ULL;
constexpr qsizetype payloadFiles = 759;
constexpr quint64 byteLimit = payloadBytes + payloadBytes / 10;
constexpr qsizetype fileCountLimit = payloadFiles + 100;
constexpr qint64 fileLimit = 2LL * 1024 * 1024 * 1024;
constexpr int extractionDeadline = 300000;
constexpr int operationDeadline = 900000;
constexpr auto installerHash = "34bb864018ad0c46a06a26d8bf0b4c48cd4bcf4c8db971c64b2ebda1b6fbdca7";

[[noreturn]] void reject(const QString& message) {
    throw std::runtime_error(message.toStdString());
}
void checked(bool value, const QString& message) { if (!value) reject(message); }
void checkDeadline(QDeadlineTimer deadline) {
    checked(!deadline.hasExpired(), "Native extraction/import exceeded its absolute deadline");
}

void copyTrustedInstaller(const QString& selected, const QString& snapshot, QDeadlineTimer deadline) {
    checkDeadline(deadline);
    // Open without following a link or blocking on a FIFO, then retain this inode.
    const int fd = ::open(QFile::encodeName(selected).constData(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    checked(fd >= 0, "Cannot safely open installer");
    QFile input;
    if (!input.open(fd, QIODevice::ReadOnly, QFileDevice::AutoCloseHandle)) {
        ::close(fd);
        reject("Cannot read installer descriptor");
    }
    struct stat info{};
    checked(::fstat(fd, &info) == 0 && S_ISREG(info.st_mode) &&
            info.st_size >= 0 && info.st_size <= installerBytes, "Installer exceeds trusted input budget or is not regular");
    QFile output(snapshot);
    checked(output.open(QIODevice::WriteOnly | QIODevice::NewOnly), "Cannot create private installer snapshot");
    checked(output.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner), "Cannot protect installer snapshot");
    std::array<char, 128 * 1024> buffer{};
    qint64 bytes = 0;
    for (;;) {
        checkDeadline(deadline);
        const qint64 count = input.read(buffer.data(), static_cast<qint64>(buffer.size()));
        checkDeadline(deadline);
        checked(count >= 0, "Cannot read installer");
        if (!count) break;
        checked(count <= installerBytes - bytes, "Growing installer exceeds trusted input budget");
        qint64 written = 0;
        while (written < count) {
            checkDeadline(deadline);
            const qint64 amount = output.write(buffer.data() + written, count - written);
            checkDeadline(deadline);
            checked(amount > 0, "Cannot copy installer snapshot");
            written += amount;
        }
        bytes += count;
    }
    checkDeadline(deadline);
    checked(output.flush(), "Cannot flush installer snapshot");
    output.close();
    checkDeadline(deadline);
    // Hash the actual closed copy consumed by innoextract, not the selected path.
    QFile actual(snapshot);
    checked(actual.open(QIODevice::ReadOnly), "Cannot verify installer snapshot");
    QCryptographicHash hash(QCryptographicHash::Sha256);
    for (;;) {
        checkDeadline(deadline);
        const qint64 count = actual.read(buffer.data(), static_cast<qint64>(buffer.size()));
        checked(count >= 0, "Cannot hash installer snapshot");
        checkDeadline(deadline);
        if (!count) break;
        hash.addData(QByteArrayView(buffer.data(), count));
    }
    checked(bytes == installerBytes && hash.result().toHex() == installerHash,
            "Unsupported installer: only the recorded original SHA256 is accepted");
    actual.close();
    checkDeadline(deadline);
    checked(QFile::setPermissions(snapshot, QFileDevice::ReadOwner), "Cannot seal installer snapshot");
}

void validatePayload(const QString& path, bool exact,
                     QDeadlineTimer deadline = QDeadlineTimer(QDeadlineTimer::Forever)) {
    checkDeadline(deadline);
    quint64 bytes = 0;
    qsizetype files = 0;
    qsizetype entries = 0;
    QDirIterator iterator(path, QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot,
                          QDirIterator::Subdirectories);
    while (iterator.hasNext()) {
        checkDeadline(deadline);
        const QString entry = iterator.next();
        checked(++entries <= 2 * fileCountLimit, "Extracted entry count exceeds trusted budget");
        struct stat info{};
        checked(::lstat(QFile::encodeName(entry).constData(), &info) == 0, "Cannot inspect extracted output");
        if (S_ISDIR(info.st_mode)) continue;
        checked(S_ISREG(info.st_mode) && info.st_nlink == 1,
                "Extracted symlinks, hardlinks and special files are forbidden");
        checked(info.st_size >= 0 && info.st_size <= fileLimit, "Extracted file exceeds 2 GiB budget");
        checked(++files <= fileCountLimit, "Extracted file count exceeds trusted budget");
        const auto size = static_cast<quint64>(info.st_size);
        checked(size <= byteLimit - bytes, "Extracted bytes exceed trusted payload budget");
        bytes += size;
    }
    checkDeadline(deadline);
    if (exact) checked(files == payloadFiles && bytes == payloadBytes,
                       "Extracted payload does not match the known 759-file/4075408963-byte installation");
}

void runBoundedProcess(const QString& program, const QStringList& arguments,
                       const QString& payload, int deadlineMilliseconds = extractionDeadline,
                       QDeadlineTimer wholeDeadline = QDeadlineTimer(QDeadlineTimer::Forever)) {
    checkDeadline(wholeDeadline);
    checked(deadlineMilliseconds > 0, "Native extraction requires a finite positive deadline");
    QDeadlineTimer deadline(qMin(deadlineMilliseconds, extractionDeadline));
    if (wholeDeadline < deadline) deadline = wholeDeadline;
    QProcess process;
    process.setProgram(program);
    process.setArguments(arguments);
    process.setWorkingDirectory(payload);
    process.setStandardOutputFile(QProcess::nullDevice());
    process.setStandardErrorFile(QProcess::nullDevice());
    process.setChildProcessModifier([] {
        // No allocation/Qt calls after fork. Fail closed if a limit is unavailable.
        const rlimit file{static_cast<rlim_t>(fileLimit), static_cast<rlim_t>(fileLimit)};
        const rlimit memory{2ULL * 1024 * 1024 * 1024, 2ULL * 1024 * 1024 * 1024};
        const rlimit cpu{300, 300};
        const rlimit core{0, 0};
        if (::setrlimit(RLIMIT_FSIZE, &file) || ::setrlimit(RLIMIT_AS, &memory) ||
            ::setrlimit(RLIMIT_CPU, &cpu) || ::setrlimit(RLIMIT_CORE, &core)) ::_exit(127);
    });
    checkDeadline(deadline);
    process.start();
    try {
        checked(process.waitForStarted(static_cast<int>(qMin<qint64>(deadline.remainingTime(), 10000))),
                "Cannot start bounded native extractor");
        while (process.state() != QProcess::NotRunning) {
            checkDeadline(deadline);
            validatePayload(payload, false, deadline);
            checkDeadline(deadline);
            process.waitForFinished(static_cast<int>(qMin<qint64>(deadline.remainingTime(), 50)));
        }
        checkDeadline(deadline);
        checked(process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0,
                "Native extraction failed (including kernel resource-limit enforcement)");
        validatePayload(payload, false, deadline);
    } catch (...) {
        if (process.state() != QProcess::NotRunning) {
            process.kill();
            process.waitForFinished(-1); // Reap before QTemporaryDir removes its output.
        }
        throw;
    }
}

QString nativeProgram(const QString& selected) {
    const QFileInfo info(selected);
    checked(info.fileName() == "innoextract" && info.isFile() && info.isExecutable(),
            "Select the native innoextract executable (not Wine or a script)");
    QFile binary(info.absoluteFilePath());
    checked(binary.open(QIODevice::ReadOnly) && binary.read(4) == QByteArray("\x7f" "ELF", 4),
            "Native ELF innoextract is required");
    return info.canonicalFilePath();
}
} // namespace

Result extractAndImport(const QString& installer, const QString& installationRoot,
                        const QString& nativeExtractor) {
    const QDeadlineTimer deadline(operationDeadline);
    checked(!installationRoot.trimmed().isEmpty(), "Installation directory is required");
    checked(QDir().mkpath(installationRoot), "Cannot create installation directory");
    const QString root = QFileInfo(installationRoot).canonicalFilePath();
    const QString sources = root + "/sources";
    checked(!QFileInfo(sources).isSymLink() && QDir().mkpath(sources),
            "Cannot create safe extraction-source directory");
    // Private under-root staging shares the retention filesystem. The importer
    // permits only this managed sources subtree to overlap the installation.
    QTemporaryDir staging(sources + "/.ikv-extract-XXXXXX");
    checked(staging.isValid(), "Cannot create private extraction staging");
    checkDeadline(deadline);
    const QString snapshot = staging.path() + "/trusted-installer.exe";
    copyTrustedInstaller(installer, snapshot, deadline);
    checkDeadline(deadline);
    const QString program = nativeProgram(nativeExtractor); // Never reached for an unknown installer.
    checkDeadline(deadline);
    const QString payload = staging.path() + "/payload";
    checked(QDir().mkdir(payload), "Cannot create private extraction output");
    runBoundedProcess(program, {"--extract", "--output-dir", payload, snapshot}, payload,
                      extractionDeadline, deadline);
    checkDeadline(deadline);
    validatePayload(payload, true, deadline);
    checkDeadline(deadline);
    const auto report = launcher_assets::importAssets(payload, root, deadline);
    checked(report.ok(), "Extracted asset import rejected: " + report.errors.join("; "));
    checkDeadline(deadline);
    checked(QFileInfo(sources).isDir() && !QFileInfo(sources).isSymLink(),
            "Retained-source directory changed during extraction");
    const QString retained = sources + "/inno-" + QUuid::createUuid().toString(QUuid::Id128);
    checkDeadline(deadline);
    checked(QDir().rename(payload, retained), "Cannot promote validated extracted source");
    return {retained, report};
}

} // namespace ikv::launcher_extraction
