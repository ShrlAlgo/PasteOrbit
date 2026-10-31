#include "desktop_integration.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QSaveFile>
#include <QStorageInfo>
#include <QSysInfo>
#include <QUuid>

namespace {
QString applicationBundle() {
    // 只更新当前正在运行的标准 .app，不能从任意可执行路径推导安装目标。
    const QFileInfo bundle(QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("../..")));
    const QString path = bundle.canonicalFilePath();
    if (!path.endsWith(QStringLiteral(".app"), Qt::CaseInsensitive)
        || bundle.isSymLink()
        || QFileInfo(path + QStringLiteral("/Contents/MacOS/PasteOrbit")).canonicalFilePath()
            != QFileInfo(QCoreApplication::applicationFilePath()).canonicalFilePath()) return {};
    return path;
}
}

namespace MacIntegration {
bool canUpdateApplication() {
    const QString bundle = applicationBundle();
    return !bundle.isEmpty() && !QStorageInfo(bundle).isReadOnly()
        && QFileInfo(QFileInfo(bundle).absolutePath()).isWritable();
}

bool startUpdate(const QString &imagePath, const QString &version, const QString &failureMessage) {
    if (!canUpdateApplication() || !QFileInfo(imagePath).isFile()) return false;
    const QString scriptPath = QDir(QDir::tempPath()).filePath(
        QStringLiteral("PasteOrbit-update-%1.sh").arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
    // 所有路径都通过参数传递；先校验暂存包，原应用只在退出后通过同卷重命名替换。
    const QByteArray script = R"SH(#!/bin/sh
set -eu
app=$1
image=$2
pid=$3
version=$4
arch=$5
message=$6
directory=${app%/*}
stage=
mount=
replaced=0
success=0
cleanup() {
    result=$?
    trap - EXIT HUP INT TERM
    if [ "$success" -ne 1 ] && [ -n "$stage" ] && [ -d "$stage/Previous.app" ]; then
        if [ "$replaced" -eq 1 ] && [ -d "$app" ]; then
            /bin/mv "$app" "$stage/Failed.app" || true
        fi
        if [ ! -e "$app" ]; then /bin/mv "$stage/Previous.app" "$app" || true; fi
    fi
    if [ -n "$mount" ]; then /usr/bin/hdiutil detach "$mount" -quiet || true; fi
    # 回滚未成功时保留备份，不删除仅剩的原应用。
    if [ -n "$stage" ] && [ ! -e "$stage/Previous.app" ]; then
        case "$stage" in "$directory"/.PasteOrbit-update.*) /bin/rm -rf "$stage";; esac
    elif [ "$success" -eq 1 ]; then
        case "$stage" in "$directory"/.PasteOrbit-update.*) /bin/rm -rf "$stage";; esac
    fi
    /bin/rm -f "$0"
    if [ "$success" -eq 1 ]; then
        /bin/rm -f "$image"
    else
        if ! /bin/kill -0 "$pid" 2>/dev/null && [ -d "$app" ]; then /usr/bin/open -n "$app" || true; fi
        /usr/bin/osascript - "$message" "$image" <<'APPLESCRIPT' || true
on run argv
    display alert "PasteOrbit" message ((item 1 of argv) & return & (item 2 of argv)) as critical
end run
APPLESCRIPT
    fi
    exit "$result"
}
trap cleanup EXIT
trap 'exit 1' HUP INT TERM
stage=$(/usr/bin/mktemp -d "$directory/.PasteOrbit-update.XXXXXX")
mount="$stage/Mount"
/bin/mkdir "$mount"
/usr/bin/hdiutil attach "$image" -readonly -nobrowse -quiet -mountpoint "$mount" </dev/null
candidate="$stage/PasteOrbit.app"
/usr/bin/ditto "$mount/PasteOrbit.app" "$candidate"
[ "$(/usr/libexec/PlistBuddy -c 'Print :CFBundleIdentifier' "$candidate/Contents/Info.plist")" = 'com.pasteorbit.PasteOrbit' ]
[ "$(/usr/libexec/PlistBuddy -c 'Print :CFBundleShortVersionString' "$candidate/Contents/Info.plist")" = "$version" ]
/usr/bin/lipo -verify_arch "$arch" "$candidate/Contents/MacOS/PasteOrbit"
/usr/bin/codesign --verify --deep --strict "$candidate"
# 最多等待 30 秒，不强制结束用户进程。
remaining=30
while /bin/kill -0 "$pid" 2>/dev/null; do
    [ "$remaining" -gt 0 ] || exit 1
    /bin/sleep 1
    remaining=$((remaining - 1))
done
/bin/mv "$app" "$stage/Previous.app"
/bin/mv "$candidate" "$app"
replaced=1
/usr/bin/open -n "$app"
success=1
)SH";
    QSaveFile scriptFile(scriptPath);
    if (!scriptFile.open(QIODevice::WriteOnly) || scriptFile.write(script) != script.size()
        || !scriptFile.commit()) return false;
    QProcess launcher;
    launcher.setProgram(QStringLiteral("/bin/sh"));
    launcher.setArguments({scriptPath, applicationBundle(), imagePath,
        QString::number(QCoreApplication::applicationPid()), version,
        QSysInfo::buildCpuArchitecture(), failureMessage});
    if (launcher.startDetached()) return true;
    QFile::remove(scriptPath);
    return false;
}
}
