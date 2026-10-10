/* SPDX-License-Identifier: GPL-2.0-or-later */
#import <Foundation/Foundation.h>
NS_ASSUME_NONNULL_BEGIN
@interface NceDevice : NSObject
@property(nonatomic, readonly) NSString *identifier;
@property(nonatomic, readonly) NSString *name;
@property(nonatomic, readonly) NSString *imageName;
@property(nonatomic, readonly) BOOL builtin;
@property(nonatomic, readonly) NSURL *directory;
@property(nonatomic, readonly) NSURL *resources;
@end

// Call on one background queue. Device metadata appears only after import completes.
@interface NceDeviceStore : NSObject
@property(nonatomic, readonly) NSURL *root;
- (instancetype)initWithRoot:(NSURL *)root resources:(NSURL *)resources;
- (nullable NSArray<NceDevice *> *)load:(NSError **)error;
- (nullable NceDevice *)createBuiltinNamed:(NSString *)name error:(NSError **)error;
- (nullable NceDevice *)importImage:(NSURL *)source raw:(BOOL)raw error:(NSError **)error;
- (BOOL)deleteDevice:(NceDevice *)device error:(NSError **)error;
- (BOOL)renameDevice:(NceDevice *)device name:(NSString *)name error:(NSError **)error;
@end
NS_ASSUME_NONNULL_END
