#include "desktop_integration.h"

#include <QDir>
#include <QFileInfo>
#include <QUrl>
#import <AppKit/AppKit.h>

namespace {
QString appName(NSRunningApplication *app) {
    NSString *name = app.bundleURL.lastPathComponent;
    if (!name.length) name = app.localizedName;
    QString result = QString::fromNSString(name);
    if (result.endsWith(QStringLiteral(".app"), Qt::CaseInsensitive)) result.chop(4);
    return result;
}
}

namespace MacIntegration {
quintptr foregroundApplication() {
    @autoreleasepool {
        return static_cast<quintptr>(NSWorkspace.sharedWorkspace.frontmostApplication.processIdentifier);
    }
}

bool isExternalApplication(quintptr process) {
    return process && process != static_cast<quintptr>(NSProcessInfo.processInfo.processIdentifier);
}

quint32 clipboardSequence() {
    @autoreleasepool {
        return static_cast<quint32>(NSPasteboard.generalPasteboard.changeCount);
    }
}

QString clipboardSource() {
    @autoreleasepool {
        auto *app = NSWorkspace.sharedWorkspace.frontmostApplication;
        if (!app || !isExternalApplication(static_cast<quintptr>(app.processIdentifier))) return {};
        return appName(app).toCaseFolded();
    }
}

QHash<QString, QString> runningApplications() {
    @autoreleasepool {
        QHash<QString, QString> result;
        for (NSRunningApplication *app in NSWorkspace.sharedWorkspace.runningApplications) {
            if (app.activationPolicy != NSApplicationActivationPolicyRegular
                || !isExternalApplication(static_cast<quintptr>(app.processIdentifier))) continue;
            const QString name = appName(app);
            if (!name.isEmpty()) result.insert(name, QString::fromNSString(app.localizedName));
        }
        return result;
    }
}

bool revealFile(const QString &path) {
    @autoreleasepool {
        if (!QDir::isAbsolutePath(path) || !QFileInfo::exists(path)) return false;
        NSURL *url = QUrl::fromLocalFile(path).toNSURL();
        if (QFileInfo(path).isDir()) return [NSWorkspace.sharedWorkspace openURL:url];
        [NSWorkspace.sharedWorkspace activateFileViewerSelectingURLs:@[url]];
        return true;
    }
}

bool animationsEnabled() {
    return !NSWorkspace.sharedWorkspace.accessibilityDisplayShouldReduceMotion;
}

void setAlwaysOnTop(quintptr view, bool pinned) {
    NSView *nativeView = (__bridge NSView *)(reinterpret_cast<void *>(view));
    nativeView.window.level = pinned ? NSFloatingWindowLevel : NSNormalWindowLevel;
}

void activateApplication() {
    [NSRunningApplication.currentApplication activateWithOptions:NSApplicationActivateIgnoringOtherApps];
}
}
