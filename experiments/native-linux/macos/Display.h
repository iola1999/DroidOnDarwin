/* SPDX-License-Identifier: GPL-2.0-or-later */
#import <AppKit/AppKit.h>
#import <QuartzCore/CAMetalLayer.h>

@interface NceDisplay : NSView
@property(nonatomic) BOOL inputEnabled;
@property(nonatomic, readonly) CAMetalLayer *_Nonnull metalLayer;
- (instancetype _Nonnull)initWithFrame:(NSRect)frame metalLayer:(CAMetalLayer *_Nonnull)layer;
- (void)navigate:(id _Nullable)sender;
@end
