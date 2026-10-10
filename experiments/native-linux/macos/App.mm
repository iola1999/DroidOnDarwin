/* SPDX-License-Identifier: GPL-2.0-or-later */
#import "Display.h"
#import "Storage.h"
#include "../graphics/host-graphics.h"
#include "../session.h"
#include <fcntl.h>
#include <unistd.h>
extern "C" int nce_lkl_run(const char *path);

typedef NS_ENUM(NSInteger, NcePhase) { NcePreparing,
                                       NceBooting,
                                       NceRunning,
                                       NceStopping,
                                       NceFinished };

@interface NceApplication : NSObject <NSApplicationDelegate, NSWindowDelegate>
@property(nonatomic) NSURL *dataDirectory;
@end

@implementation NceApplication {
    NSWindow *_window;
    NceDisplay *_display;
    NceStorage *_storage;
    NSMenuItem *_status;
    NSArray<NSButton *> *_navigation;
    dispatch_source_t _logTimer;
    NcePhase _phase;
    BOOL _terminationPending;
}

- (NSMenuItem *)item:(NSString *)title action:(SEL)action key:(NSString *)key menu:(NSMenu *)menu
{
    NSMenuItem *item = [[NSMenuItem alloc] initWithTitle:title action:action keyEquivalent:key];
    item.target = self;
    [menu addItem:item];
    return item;
}

- (void)applicationDidFinishLaunching:(NSNotification *)notification
{
    (void)notification;
    NSMenu *main = [NSMenu new];
    NSMenu *application = [NSMenu new];
    NSMenuItem *applicationItem = [main addItemWithTitle:@"Husk NCE" action:nil keyEquivalent:@""];
    applicationItem.submenu = application;
    [self item:@"关于 Husk NCE" action:@selector(about:) key:@"" menu:application];
    [application addItem:NSMenuItem.separatorItem];
    [self item:@"退出 Husk NCE" action:@selector(quit:) key:@"q" menu:application];
    NSMenu *android = [NSMenu new];
    android.autoenablesItems = NO;
    [main addItemWithTitle:@"Android" action:nil keyEquivalent:@""].submenu = android;
    _status = [self item:@"正在准备 Android" action:nil key:@"" menu:android];
    _status.enabled = NO;
    [android addItem:NSMenuItem.separatorItem];
    [self item:@"打开日志目录" action:@selector(showLogs:) key:@"l" menu:android];
    [self item:@"打开数据目录" action:@selector(showData:) key:@"" menu:android];
    [android addItem:NSMenuItem.separatorItem];
    [self item:@"停止并退出" action:@selector(quit:) key:@"" menu:android];
    NSApp.mainMenu = main;

    _window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 360, 684)
                                          styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                                                    NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable
                                            backing:NSBackingStoreBuffered
                                              defer:NO];
    _window.title = @"Android";
    _window.delegate = self;
    _window.releasedWhenClosed = NO;
    _window.contentMinSize = NSMakeSize(270, 524);
    _window.backgroundColor = NSColor.windowBackgroundColor;
    _display = [[NceDisplay alloc] initWithFrame:NSMakeRect(0, 44, 360, 640)];
    _display.translatesAutoresizingMaskIntoConstraints = NO;
    NSView *content = _window.contentView;
    [content addSubview:_display];
    NSMutableArray *buttons = [NSMutableArray new];
    NSArray *titles = @[ @"返回", @"主页", @"最近应用" ];
    const int keys[] = {158, 102, 580};
    for (NSUInteger i = 0; i < titles.count; ++i) {
        NSButton *button = [NSButton buttonWithTitle:titles[i] target:_display action:@selector(navigate:)];
        button.tag = keys[i];
        button.enabled = NO;
        button.bezelStyle = NSBezelStyleRounded;
        [buttons addObject:button];
    }
    _navigation = buttons;
    NSStackView *navigation = [NSStackView stackViewWithViews:buttons];
    navigation.translatesAutoresizingMaskIntoConstraints = NO;
    navigation.distribution = NSStackViewDistributionFillEqually;
    navigation.spacing = 8;
    [content addSubview:navigation];
    NSLayoutConstraint *width = [_display.widthAnchor constraintEqualToAnchor:content.widthAnchor];
    width.priority = NSLayoutPriorityDefaultHigh;
    NSLayoutConstraint *height = [_display.heightAnchor constraintEqualToAnchor:content.heightAnchor constant:-44];
    height.priority = NSLayoutPriorityDefaultHigh;
    [NSLayoutConstraint activateConstraints:@[
        [_display.centerXAnchor constraintEqualToAnchor:content.centerXAnchor],
        [_display.centerYAnchor constraintEqualToAnchor:content.centerYAnchor
                                               constant:-22],
        [_display.widthAnchor constraintEqualToAnchor:_display.heightAnchor
                                           multiplier:360.0 / 640],
        [_display.widthAnchor constraintLessThanOrEqualToAnchor:content.widthAnchor],
        [_display.heightAnchor constraintLessThanOrEqualToAnchor:content.heightAnchor
                                                        constant:-44],
        width, height,
        [navigation.leadingAnchor constraintEqualToAnchor:content.leadingAnchor
                                                 constant:12],
        [navigation.trailingAnchor constraintEqualToAnchor:content.trailingAnchor
                                                  constant:-12],
        [navigation.bottomAnchor constraintEqualToAnchor:content.bottomAnchor
                                                constant:-8],
        [navigation.heightAnchor constraintEqualToConstant:28]
    ]];
    [_window center];
    [_window makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];
    if (!_display.metalLayer.device) {
        [self fail:@"无法初始化 Metal 显示。" error:nil];
        return;
    }
    _phase = NcePreparing;
    _storage = [NceStorage new];
    NSURL *resources = NSBundle.mainBundle.resourceURL;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
      NSError *error = nil;
      BOOL prepared = [self->_storage prepareResources:resources directory:self.dataDirectory error:&error];
      dispatch_async(dispatch_get_main_queue(), ^{
        if (self->_phase == NceStopping) {
            [self finish:0];
            return;
        }
        if (!prepared) {
            [self fail:@"无法启动 Android" error:error];
            return;
        }
        [self start:resources];
      });
    });
}

- (void)start:(NSURL *)resources
{
    int log = open(_storage.log.fileSystemRepresentation, O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC, 0600);
    if (log < 0) {
        [self fail:@"无法创建运行日志，请检查数据目录权限。" error:nil];
        return;
    }
    if (dup2(log, STDOUT_FILENO) < 0 || dup2(log, STDERR_FILENO) < 0) {
        close(log);
        [self fail:@"无法打开运行日志。" error:nil];
        return;
    }
    close(log);
    setvbuf(stdout, NULL, _IOLBF, 0);
    setvbuf(stderr, NULL, _IOLBF, 0);
    setenv("NCE_STORAGE_IMAGE", _storage.disk.fileSystemRepresentation, 1);
    setenv("NCE_SYSTEM_IMAGE", [resources URLByAppendingPathComponent:@"system.img"].fileSystemRepresentation, 1);
    setenv("NCE_DESKTOP", "1", 1);
    setenv("NCE_MM_MODE", "demand", 1);
    setenv("ANGLE_DEFAULT_PLATFORM", "metal", 1);
    _phase = NceBooting;
    _status.title = @"正在启动 Android";
    nce_graphics_set_window((__bridge void *)_display.metalLayer);
    [self observeLog];
    NSString *kernel = [resources URLByAppendingPathComponent:@"lkl-image.bin"].path;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
      int result = nce_lkl_run(kernel.fileSystemRepresentation);
      fflush(stdout);
      fflush(stderr);
      dispatch_async(dispatch_get_main_queue(), ^{
        [self finish:result];
      });
    });
}

- (void)observeLog
{
    int fd = open(_storage.log.fileSystemRepresentation, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return;
    dispatch_queue_t queue = dispatch_queue_create("app.husk.mac.status", DISPATCH_QUEUE_SERIAL);
    _logTimer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, queue);
    dispatch_source_set_timer(_logTimer, DISPATCH_TIME_NOW, NSEC_PER_SEC / 2, NSEC_PER_SEC / 10);
    NSMutableData *pending = [NSMutableData new];
    dispatch_source_set_event_handler(_logTimer, ^{
      char bytes[65536];
      ssize_t count = read(fd, bytes, sizeof(bytes));
      if (count <= 0)
          return;
      [pending appendBytes:bytes length:count];
      NSData *marker = [@"{\"phase\":\"android_desktop_running\"}" dataUsingEncoding:NSUTF8StringEncoding];
      BOOL ready = [pending rangeOfData:marker options:0 range:NSMakeRange(0, pending.length)].location != NSNotFound;
      if (pending.length > marker.length)
          [pending replaceBytesInRange:NSMakeRange(0, pending.length - marker.length) withBytes:NULL length:0];
      if (ready)
          dispatch_async(dispatch_get_main_queue(), ^{
            if (self->_phase == NceBooting) {
                self->_phase = NceRunning;
                self->_status.title = @"Android 正在运行";
                self->_display.inputEnabled = YES;
                for (NSButton *button in self->_navigation)
                    button.enabled = YES;
                [self stopObserving];
            }
          });
    });
    dispatch_source_set_cancel_handler(_logTimer, ^{
      close(fd);
    });
    dispatch_resume(_logTimer);
}

- (void)stopObserving
{
    if (_logTimer) {
        dispatch_source_cancel(_logTimer);
        _logTimer = nil;
    }
}
- (void)finish:(int)result
{
    _phase = NceFinished;
    _display.inputEnabled = NO;
    for (NSButton *button in _navigation)
        button.enabled = NO;
    [self stopObserving];
    _status.title = result ? @"Android 运行失败" : @"Android 已停止";
    // 启动失败时内核资源可能仍在使用，目录锁保留到宿主进程退出。
    if (_terminationPending)
        [NSApp replyToApplicationShouldTerminate:YES];
    else if (result)
        [self fail:@"Android 运行失败，请通过 Android 菜单查看日志。" error:nil];
}
- (void)fail:(NSString *)message error:(NSError *)error
{
    _phase = NceFinished;
    _status.title = @"Android 启动失败";
    NSAlert *alert = [NSAlert new];
    alert.messageText = message;
    alert.informativeText = error.localizedDescription ?: @"退出后可以重新打开应用。";
    [alert addButtonWithTitle:@"好"];
    [alert beginSheetModalForWindow:_window completionHandler:nil];
}
- (void)requestStop
{
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
      int result = nce_session_request_stop();
      dispatch_async(dispatch_get_main_queue(), ^{
        // Linux 初始化早期还没有注册停止入口，需要稍后重试。
        if (result && self->_phase == NceStopping)
            dispatch_after(dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC / 2), dispatch_get_main_queue(), ^{
              if (self->_phase == NceStopping)
                  [self requestStop];
            });
      });
    });
}
- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication *)sender
{
    (void)sender;
    if (_phase == NceFinished)
        return NSTerminateNow;
    if (_terminationPending)
        return NSTerminateLater;
    _terminationPending = YES;
    BOOL preparing = _phase == NcePreparing;
    _phase = NceStopping;
    _status.title = @"正在停止 Android";
    _display.inputEnabled = NO;
    for (NSButton *button in _navigation)
        button.enabled = NO;
    if (!preparing)
        [self requestStop];
    return NSTerminateLater;
}
- (BOOL)windowShouldClose:(NSWindow *)window
{
    (void)window;
    [NSApp terminate:nil];
    return NO;
}
- (BOOL)applicationShouldHandleReopen:(NSApplication *)sender hasVisibleWindows:(BOOL)visible
{
    (void)sender;
    if (!visible)
        [_window makeKeyAndOrderFront:nil];
    return YES;
}
- (void)quit:(id)sender
{
    [NSApp terminate:sender];
}
- (void)showData:(id)sender
{
    (void)sender;
    [NSWorkspace.sharedWorkspace openURL:self.dataDirectory];
}
- (void)showLogs:(id)sender
{
    (void)sender;
    [NSWorkspace.sharedWorkspace openURL:[self.dataDirectory URLByAppendingPathComponent:@"Logs"]];
}
- (void)about:(id)sender
{
    (void)sender;
    [NSApp orderFrontStandardAboutPanel:nil];
}
@end

int main(int argc, const char **argv)
{
    @autoreleasepool {
        NSApplication *application = NSApplication.sharedApplication;
        [application setActivationPolicy:NSApplicationActivationPolicyRegular];
        NceApplication *delegate = [NceApplication new];
        NSURL *base = [NSFileManager.defaultManager URLsForDirectory:NSApplicationSupportDirectory inDomains:NSUserDomainMask].firstObject;
        delegate.dataDirectory = [base URLByAppendingPathComponent:@"app.husk.nce.mac" isDirectory:YES];
        for (int i = 1; i < argc; ++i) {
            if (!strcmp(argv[i], "--data-directory") && i + 1 < argc) {
                delegate.dataDirectory = [NSURL fileURLWithPath:[NSString stringWithUTF8String:argv[++i]] isDirectory:YES];
            } else {
                fprintf(stderr, "Usage: HuskNCE [--data-directory PATH]\n");
                return 2;
            }
        }
        application.delegate = delegate;
        [application run];
    }
    return 0;
}
