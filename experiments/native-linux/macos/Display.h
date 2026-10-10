/* SPDX-License-Identifier: GPL-2.0-or-later */
#import <AppKit/AppKit.h>
#import <QuartzCore/CAMetalLayer.h>

@interface NceDisplay : NSView
@property(nonatomic) BOOL inputEnabled;
@property(nonatomic, readonly) CAMetalLayer *metalLayer;
- (void)navigate:(id)sender;
@end
