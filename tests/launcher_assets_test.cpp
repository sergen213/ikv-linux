#include "ikv/launcher_assets.hpp"

#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <iostream>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>

namespace {
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
void put(const QString& path, const QByteArray& contents) {
    require(QDir().mkpath(QFileInfo(path).absolutePath()), "create fixture directory");
    QFile file(path);
    require(file.open(QIODevice::WriteOnly) && file.write(contents) == contents.size(), "write fixture");
}
QByteArray get(const QString& path) {
    QFile file(path);
    require(file.open(QIODevice::ReadOnly), "read fixture");
    return file.readAll();
}
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    try {
        namespace assets = ikv::launcher_assets;
        QTemporaryDir temporary;
        require(temporary.isValid(), "temporary directory");
        const QString source = temporary.path() + "/source/app";
        const QString install = temporary.path() + "/install";
        // Structurally valid SKYF v1; synthetic opaque payload, never game data.
        QByteArray archive("SKYF\x01\x00", 6);
        archive += QByteArray("textures", 8) + QByteArray(56, '\0');
        archive += QByteArray("\x00\x00\x01\x00", 4);
        archive += QByteArray("fixture.dds", 11) + QByteArray(117, '\0');
        archive += QByteArray("\xd2\x00\x00\x00\x04\x00\x00\x00", 8);
        archive += QByteArray("data", 4);
        put(source + "/fixture.sky", archive);
        put(source + "/nested/unchanged.bin", "original");
        put(install + "/settings.ini", "user-settings");
        const auto imported = assets::importAssets(temporary.path() + "/source", install);
        require(imported.ok(), "recognizable archive import");
        require(get(source + "/fixture.sky") == archive, "source preserved");
        require(get(assets::assetRoot(install) + "/fixture.sky") == archive, "actual archive copied");
        require(assets::verifyAssets(install).ok(), "imported snapshot verifies");
        require(!assets::importAssets(source, install).ok(), "existing installation not overwritten");
        const auto entryFilters = QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot;
        const QStringList installEntries = QDir(install).entryList(entryFilters);
        const QStringList assetEntries = QDir(assets::assetRoot(install)).entryList(entryFilters);
        const QStringList sourceEntries = QDir(source).entryList(entryFilters);
        const QByteArray deadlineManifest = get(assets::manifestPath(install));
        const auto expiredExisting = assets::importAssets(source, install, QDeadlineTimer(0));
        require(!expiredExisting.ok() &&
                expiredExisting.errors.join('\n').contains("deadline", Qt::CaseInsensitive),
                "expired deadline explicitly rejects existing installation");
        require(get(assets::assetRoot(install) + "/fixture.sky") == archive &&
                get(assets::assetRoot(install) + "/nested/unchanged.bin") == "original",
                "expired deadline preserves existing assets");
        require(get(assets::manifestPath(install)) == deadlineManifest,
                "expired deadline preserves existing manifest");
        require(get(install + "/settings.ini") == "user-settings",
                "expired deadline preserves existing settings");
        require(QDir(install).entryList(entryFilters) == installEntries &&
                QDir(assets::assetRoot(install)).entryList(entryFilters) == assetEntries,
                "expired deadline leaves no existing-install staging remnants");
        require(get(source + "/fixture.sky") == archive &&
                get(source + "/nested/unchanged.bin") == "original" &&
                QDir(source).entryList(entryFilters) == sourceEntries,
                "expired deadline preserves source snapshot");
        const QString deadlineInstall = temporary.path() + "/deadline-install";
        put(deadlineInstall + "/settings.ini", "deadline-settings");
        const QStringList deadlineEntries = QDir(deadlineInstall).entryList(entryFilters);
        const auto expiredEmpty = assets::importAssets(source, deadlineInstall, QDeadlineTimer(0));
        require(!expiredEmpty.ok() &&
                expiredEmpty.errors.join('\n').contains("deadline", Qt::CaseInsensitive),
                "expired deadline explicitly rejects installation without assets");
        require(!QFileInfo::exists(assets::assetRoot(deadlineInstall)) &&
                !QFileInfo::exists(assets::manifestPath(deadlineInstall)) &&
                QDir(deadlineInstall).entryList(entryFilters) == deadlineEntries,
                "expired deadline publishes no assets or manifest and retains no staging");
        require(get(deadlineInstall + "/settings.ini") == "deadline-settings",
                "expired deadline preserves settings without assets");
        require(get(source + "/fixture.sky") == archive &&
                get(source + "/nested/unchanged.bin") == "original" &&
                QDir(source).entryList(entryFilters) == sourceEntries,
                "expired deadline without assets preserves source snapshot");
        const auto finiteImport = assets::importAssets(source, deadlineInstall, QDeadlineTimer(300000));
        require(finiteImport.ok() && assets::verifyAssets(deadlineInstall).ok(),
                "generous finite deadline imports a verifiable snapshot");
        require(get(assets::assetRoot(deadlineInstall) + "/fixture.sky") == archive &&
                get(assets::assetRoot(deadlineInstall) + "/nested/unchanged.bin") == "original",
                "finite deadline copies actual source files");
        require(get(deadlineInstall + "/settings.ini") == "deadline-settings" &&
                get(source + "/fixture.sky") == archive &&
                get(source + "/nested/unchanged.bin") == "original",
                "finite deadline preserves settings and source");
        put(assets::assetRoot(install) + "/fixture.sky", "damaged");
        require(QFile::remove(assets::assetRoot(install) + "/nested/unchanged.bin"), "remove imported fixture");
        put(assets::assetRoot(install) + "/extra.bin", "retain");
        const auto damaged = assets::verifyAssets(install);
        require(damaged.changed.contains("fixture.sky") && damaged.missing.contains("nested/unchanged.bin") &&
                damaged.extra.contains("extra.bin"), "changed missing and extra reported");
        put(source + "/fixture.sky", "changed-source");
        require(!assets::repairAssets(source, install).ok(), "changed repair source refused");
        require(get(assets::assetRoot(install) + "/fixture.sky") == "damaged", "failed repair preserves target");
        put(source + "/fixture.sky", archive);
        const auto repaired = assets::repairAssets(source, install);
        require(repaired.errors.isEmpty() && repaired.changed.isEmpty() && repaired.missing.isEmpty() &&
                repaired.extra.contains("extra.bin"), "repair restores manifest only and reports extras");
        require(get(assets::assetRoot(install) + "/extra.bin") == "retain", "extras preserved");
        require(get(install + "/settings.ini") == "user-settings", "settings preserved");
        require(QFile::remove(assets::assetRoot(install) + "/extra.bin"), "remove extra");
        require(assets::verifyAssets(install).ok(), "repair verifies");
        const QString empty = temporary.path() + "/empty";
        require(QDir().mkpath(empty), "empty source");
        require(!assets::importAssets(empty, temporary.path() + "/empty-install").ok(), "empty source refused");
        put(empty + "/fake.sky", "not-an-archive");
        require(!assets::importAssets(empty, temporary.path() + "/fake-install").ok(), "fake archive refused");
        const QByteArray linkPath = QFile::encodeName(source + "/unsafe");
        require(::symlink("/etc/passwd", linkPath.constData()) == 0, "symlink fixture");
        require(!assets::importAssets(source, temporary.path() + "/unsafe-install").ok(), "symlink source refused");
        require(QFile::remove(source + "/unsafe"), "remove symlink");
        const QByteArray fifoPath = QFile::encodeName(source + "/unsafe-fifo");
        require(::mkfifo(fifoPath.constData(), 0600) == 0, "special file fixture");
        require(!assets::importAssets(source, temporary.path() + "/fifo-install").ok(), "special file refused");
        require(QFile::remove(source + "/unsafe-fifo"), "remove special file");
        const QString deep = temporary.path() + "/deep";
        put(deep + "/fixture.sky", archive);
        QString nested = deep;
        for (int i = 0; i < 50; ++i) nested += "/level";
        put(nested + "/file", "bounded");
        require(!assets::importAssets(deep, temporary.path() + "/deep-install").ok(), "deep traversal bounded");
        require(!assets::importAssets(source, source + "/installation").ok(), "source overlap refused");
        const QString retainedInstall = temporary.path() + "/retained-install";
        const QString retainedContainer = retainedInstall + "/sources/original-snapshot";
        const QString retainedSource = retainedContainer + "/app/base";
        put(retainedSource + "/fixture.sky", archive);
        put(retainedInstall + "/settings.ini", "retained-settings");
        require(assets::importAssets(retainedContainer, retainedInstall).ok(), "retained source import");
        put(assets::assetRoot(retainedInstall) + "/fixture.sky", "damaged-retained-copy");
        require(assets::repairAssets(retainedContainer, retainedInstall).ok(), "retained source repair");
        require(get(retainedSource + "/fixture.sky") == archive, "retained original preserved");
        require(get(retainedInstall + "/settings.ini") == "retained-settings", "retained settings preserved");
        require(!assets::importAssets(retainedInstall, retainedInstall).ok(), "same directory overlap refused");
        require(!assets::repairAssets(retainedInstall + "/sources", retainedInstall).ok(), "sources container itself refused");
        for (const QString& reserved : QStringList{"assets", "releases", "settings"}) {
            const QString root = temporary.path() + "/invalid-" + reserved;
            const QString reservedSource = root + '/' + reserved + "/app/base";
            put(reservedSource + "/fixture.sky", archive);
            require(!assets::importAssets(reservedSource, root).ok(), "ordinary install descendant refused");
        }
        const QByteArray retainedUnsafe = QFile::encodeName(retainedSource + "/unsafe");
        require(::symlink("/etc/passwd", retainedUnsafe.constData()) == 0, "retained symlink fixture");
        require(!assets::repairAssets(retainedContainer, retainedInstall).ok(), "retained source symlink refused");
        require(QFile::remove(retainedSource + "/unsafe"), "remove retained symlink");
        const QByteArray retainedFifo = QFile::encodeName(retainedSource + "/unsafe-fifo");
        require(::mkfifo(retainedFifo.constData(), 0600) == 0, "retained special file fixture");
        require(!assets::repairAssets(retainedContainer, retainedInstall).ok(), "retained source special file refused");
        require(QFile::remove(retainedSource + "/unsafe-fifo"), "remove retained special file");
        const QString linkedContainer = retainedInstall + "/sources/linked";
        require(::symlink(QFile::encodeName(retainedContainer).constData(),
                          QFile::encodeName(linkedContainer).constData()) == 0, "retained ancestor symlink fixture");
        require(!assets::repairAssets(linkedContainer, retainedInstall).ok(), "retained ancestor symlink refused");
        // Tampered manifests cannot target launcher settings or absolute paths.
        const QString manifest = assets::manifestPath(install);
        QByteArray originalManifest = get(manifest);
        QByteArray tampered = originalManifest;
        tampered.replace("nested/unchanged.bin", "../settings.ini");
        put(manifest, tampered);
        require(!assets::repairAssets(source, install).ok(), "manifest traversal refused");
        require(get(install + "/settings.ini") == "user-settings", "tampered manifest preserves settings");
        std::cout << "launcher asset import/verify/repair checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
