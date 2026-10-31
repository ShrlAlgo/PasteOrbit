#include "desktop_integration.h"

#include <QDir>
#include <QFileInfo>
#include <QUrl>
#include <QPointer>
#include <QTimer>
#include <Carbon/Carbon.h>
#import <AppKit/AppKit.h>

@interface PasteOrbitReopenHandler : NSObject
@property(nonatomic, copy) void (^reopen)(void);
- (void)handleReopen:(NSAppleEventDescriptor *)event withReplyEvent:(NSAppleEventDescriptor *)reply;
@end

@implementation PasteOrbitReopenHandler
- (void)handleReopen:(NSAppleEventDescriptor *)event withReplyEvent:(NSAppleEventDescriptor *)reply {
    Q_UNUSED(event);
    Q_UNUSED(reply);
    if (self.reopen) self.reopen();
}
@end

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

// 只监听 Dock/Finder 的重新打开事件，菜单栏点击和普通激活不唤出面板。
void installDockReopenHandler(QObject *context, std::function<void()> reopen) {
    auto *handler = [[PasteOrbitReopenHandler alloc] init];
    const QPointer<QObject> guard(context);
    handler.reopen = ^{
        if (guard) QTimer::singleShot(0, guard.data(), reopen);
    };
    // AppKit 完成启动后再安装，避免启动阶段覆盖自定义事件处理。
    QTimer::singleShot(0, context, [handler] {
        [NSAppleEventManager.sharedAppleEventManager setEventHandler:handler
            andSelector:@selector(handleReopen:withReplyEvent:)
            forEventClass:kCoreEventClass andEventID:kAEReopenApplication];
    });
    QObject::connect(context, &QObject::destroyed, [] {
        [NSAppleEventManager.sharedAppleEventManager removeEventHandlerForEventClass:kCoreEventClass
            andEventID:kAEReopenApplication];
    });
}

void preparePanel(quintptr view) {
    NSWindow *window = ((__bridge NSView *)(reinterpret_cast<void *>(view))).window;
    // 映射前设为当前桌面的辅助面板，避免台前调度切走当前窗口组。
    window.collectionBehavior = (window.collectionBehavior & ~NSWindowCollectionBehaviorCanJoinAllSpaces)
        | NSWindowCollectionBehaviorMoveToActiveSpace | NSWindowCollectionBehaviorFullScreenAuxiliary;
}

void activateApplication(quintptr view) {
    if (view) preparePanel(view);
    [NSRunningApplication.currentApplication activateWithOptions:NSApplicationActivateIgnoringOtherApps];
}
}
