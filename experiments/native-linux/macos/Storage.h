/* SPDX-License-Identifier: GPL-2.0-or-later */
#import <Foundation/Foundation.h>

@interface NceStorage : NSObject
@property(nonatomic, readonly) NSURL *directory;
@property(nonatomic, readonly) NSURL *disk;
@property(nonatomic, readonly) NSURL *log;
/* Retain through Linux shutdown: this object owns the data-directory lock. */
- (BOOL)prepareResources:(NSURL *)resources directory:(NSURL *)directory error:(NSError **)error;
@end
