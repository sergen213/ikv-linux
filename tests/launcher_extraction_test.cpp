// Include the implementation to exercise its private process/validation helpers
// without adding a production trust-gate bypass. Link QtCore and launcher-assets.
#include "../src/launcher_extraction.cpp"

#include <QCoreApplication>
#include <iostream>

namespace {
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
void put(const QString& path, const QByteArray& bytes) {
    QFile file(path);
    require(file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size(), "write fixture");
}
template<typename Function> void rejects(Function function, const char* message) {
    bool rejected = false;
    try { function(); } catch (const std::exception&) { rejected = true; }
    require(rejected, message);
}
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    try {
        using namespace ikv::launcher_extraction;
        QTemporaryDir temporary;
        require(temporary.isValid(), "temporary directory");
        const QString root = temporary.path() + "/install";
        const QString installer = temporary.path() + "/unknown.exe";
        const QString marker = temporary.path() + "/invoked";
        const QString extractor = temporary.path() + "/innoextract";
        put(installer, "unknown installer bytes");
        put(extractor, "#!/bin/sh\ntouch '" + marker.toUtf8() + "'\n");
        require(QFile::setPermissions(extractor, QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                                QFileDevice::ExeOwner), "executable fixture");
        bool hashRejected = false;
        try { extractAndImport(installer, root, extractor); }
        catch (const std::exception& error) { hashRejected = QString::fromUtf8(error.what()).contains("SHA256"); }
        require(hashRejected, "unknown hash rejected before extractor validation");
        require(!QFileInfo::exists(marker), "unknown installer must not invoke extractor");
        require(!QFileInfo::exists(root + "/assets"), "unknown installer not imported");
        require(QDir(root + "/sources").entryList({".ikv-extract-*"},
                QDir::Dirs | QDir::Hidden).isEmpty(), "rejected installer staging cleaned");
        require(!QFileInfo::exists(root + "/sources") ||
                QDir(root + "/sources").entryList(QDir::AllEntries | QDir::Hidden |
                                                  QDir::NoDotAndDotDot).isEmpty(), "no rejected retained source");

        const QDeadlineTimer expired(0);
        {
            QTemporaryDir expiredStage(temporary.path() + "/.ikv-extract-XXXXXX");
            require(expiredStage.isValid(), "expired operation staging fixture");
            const QString snapshot = expiredStage.path() + "/trusted-installer.exe";
            rejects([&] { copyTrustedInstaller(installer, snapshot, expired); },
                    "expired whole deadline rejects installer snapshot");
            require(!QFileInfo::exists(snapshot), "expired operation cannot copy source");
        }
        QFile original(installer);
        require(original.open(QIODevice::ReadOnly) && original.readAll() == "unknown installer bytes",
                "expired operation preserves original installer");
        require(QDir(temporary.path()).entryList({".ikv-extract-*"}, QDir::Dirs | QDir::Hidden).isEmpty(),
                "expired operation staging cleaned");

        const QString payload = temporary.path() + "/payload";
        require(QDir().mkpath(payload), "payload directory");
        put(payload + "/file", "bounded");
        rejects([&] { validatePayload(payload, false, expired); },
                "expired whole deadline rejects payload scan");
        rejects([&] { validatePayload(payload, true); }, "exact known payload required");
        const QByteArray link = QFile::encodeName(payload + "/symlink");
        require(::symlink("file", link.constData()) == 0, "symlink fixture");
        rejects([&] { validatePayload(payload, false); }, "symlink output rejected");
        require(QFile::remove(payload + "/symlink"), "remove symlink");
        require(::link(QFile::encodeName(payload + "/file").constData(), link.constData()) == 0,
                "hardlink fixture");
        rejects([&] { validatePayload(payload, false); }, "hardlink output rejected");
        require(QFile::remove(payload + "/symlink"), "remove hardlink");
        const QByteArray fifo = QFile::encodeName(payload + "/fifo");
        require(::mkfifo(fifo.constData(), 0600) == 0, "fifo fixture");
        rejects([&] { validatePayload(payload, false); }, "special output rejected");
        require(QFile::remove(payload + "/fifo"), "remove fifo");
        QFile oversized(payload + "/oversized");
        require(oversized.open(QIODevice::WriteOnly) && oversized.resize(fileLimit + 1), "sparse large fixture");
        oversized.close();
        rejects([&] { validatePayload(payload, false); }, "per-file output budget checked");
        require(QFile::remove(payload + "/oversized"), "remove large fixture");
        for (int i = 0; i < fileCountLimit; ++i) put(payload + QString("/entry-%1").arg(i), {});
        rejects([&] { validatePayload(payload, false); }, "file-count output budget checked");
        require(QDir(payload).removeRecursively() && QDir().mkpath(payload), "reset payload fixture");
        for (int i = 0; i < 3; ++i) {
            QFile aggregate(payload + QString("/aggregate-%1").arg(i));
            require(aggregate.open(QIODevice::WriteOnly) && aggregate.resize(i < 2 ? fileLimit : 250000000),
                    "aggregate sparse fixture");
        }
        rejects([&] { validatePayload(payload, false); }, "aggregate output budget checked");

        // These run the same private limit/deadline wrapper, not extractAndImport.
        const QString processRoot = temporary.path() + "/process";
        require(QDir().mkpath(processRoot), "process payload directory");
        rejects([&] { runBoundedProcess(extractor, {}, processRoot, 5000, expired); },
                "expired whole deadline rejects child before launch");
        require(!QFileInfo::exists(marker), "expired operation must not invoke extractor");
        rejects([&] { runBoundedProcess("/bin/sleep", {"5"}, processRoot, 5000, QDeadlineTimer(100)); },
                "whole operation remaining budget limits child deadline");
        rejects([&] { runBoundedProcess("/bin/sleep", {"5"}, processRoot, 100); },
                "absolute process deadline enforced");
        require(QDir(processRoot).entryList(QDir::AllEntries | QDir::NoDotAndDotDot).isEmpty(),
                "timeout output empty");
        rejects([&] { runBoundedProcess("/bin/sh", {"-c", "truncate -s 2147483649 oversized"},
                                        processRoot, 5000); }, "kernel file limit enforced");
        require(!QFileInfo::exists(processRoot + "/oversized") ||
                QFileInfo(processRoot + "/oversized").size() <= fileLimit, "kernel prevents oversized file");
        runBoundedProcess("/bin/sh", {"-c", "ulimit -t > cpu-limit"}, processRoot, 5000);
        QFile cpuLimit(processRoot + "/cpu-limit");
        require(cpuLimit.open(QIODevice::ReadOnly) && cpuLimit.readAll().trimmed() == "300",
                "finite 300-second kernel CPU budget");
        {
            QTemporaryDir rejected(temporary.path() + "/.ikv-extract-XXXXXX");
            require(rejected.isValid(), "cleanup staging fixture");
            put(rejected.path() + "/invalid", "invalid asset bytes");
            const auto report = ikv::launcher_assets::importAssets(rejected.path(),
                                                                 temporary.path() + "/invalid-install");
            require(!report.ok(), "asset validation fails before retention");
        }
        require(QDir(temporary.path()).entryList({".ikv-extract-*"}, QDir::Dirs | QDir::Hidden).isEmpty(),
                "validation failure staging cleaned");
        // Optional real fixture goes through the production gate without overrides.
        // Usage: launcher-extraction-test <trusted-installer> <native-innoextract>
        // This authorized fixture can exceed CTest's 60-second unit-test timeout;
        // run it separately with enough time for the 900-second operation budget.
        if (argc == 3) {
            const QString realRoot = temporary.path() + "/known-install";
            const auto result = extractAndImport(QString::fromLocal8Bit(argv[1]), realRoot,
                                                 QString::fromLocal8Bit(argv[2]));
            require(result.report.ok() && QFileInfo(result.sourcePath).isDir(), "real extraction imported and retained");
            validatePayload(result.sourcePath, true);
            require(ikv::launcher_assets::verifyAssets(realRoot).ok(), "real imported snapshot verifies");
            require(QDir(realRoot + "/sources").entryList({".ikv-extract-*"}, QDir::Dirs | QDir::Hidden).isEmpty(),
                    "successful extraction staging cleaned");
        }
        std::cout << "trusted extraction gate, budgets, deadline and cleanup checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
