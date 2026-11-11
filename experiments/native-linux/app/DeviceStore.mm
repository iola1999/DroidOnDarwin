/* SPDX-License-Identifier: GPL-2.0-or-later */
#import "DeviceStore.h"
#include <CommonCrypto/CommonDigest.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

@implementation NceDevice
- (instancetype)initWithRecord:(NSDictionary *)record directory:(NSURL *)directory resources:(NSURL *)resources
{
    if ((self = [super init])) {
        _identifier = record[@"id"];
        _name = record[@"name"];
        _imageName = record[@"image"];
        _builtin = [record[@"builtin"] boolValue];
        _directory = directory;
        _resources = _builtin ? resources : [directory URLByAppendingPathComponent:@"Images"];
    }
    return self;
}
@end

static BOOL fail(NSError **error, NSString *message)
{
    if (error)
        *error = [NSError errorWithDomain:@"app.droidondarwin.devices" code:1 userInfo:@{NSLocalizedDescriptionKey : message}];
    return NO;
}
static BOOL regular(NSURL *url)
{
    struct stat st;
    return lstat(url.fileSystemRepresentation, &st) == 0 && S_ISREG(st.st_mode);
}
static BOOL validRuntime(id value)
{
    return [value isKindOfClass:NSString.class] &&
           [[value stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet] length] > 0;
}
static NSDictionary *json(NSURL *url, NSError **error)
{
    if (!regular(url)) {
        fail(error, @"配置文件缺失或类型错误。");
        return nil;
    }
    NSDictionary *attributes = [NSFileManager.defaultManager attributesOfItemAtPath:url.path error:error];
    if ([attributes[NSFileSize] unsignedLongLongValue] > 65536) {
        fail(error, @"配置文件超过大小限制。");
        return nil;
    }
    NSData *data = [NSData dataWithContentsOfURL:url options:0 error:error];
    id value = data ? [NSJSONSerialization JSONObjectWithData:data options:0 error:error] : nil;
    if (![value isKindOfClass:NSDictionary.class]) {
        fail(error, @"配置文件格式错误。");
        return nil;
    }
    return value;
}
static BOOL ext4(NSURL *url, NSError **error)
{
    int fd = open(url.fileSystemRepresentation, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    struct stat st = {};
    unsigned char super[64] = {};
    BOOL valid = fd >= 0 && fstat(fd, &st) == 0 && S_ISREG(st.st_mode) &&
                 pread(fd, super, sizeof(super), 1024) == sizeof(super) && super[56] == 0x53 && super[57] == 0xef;
    if (fd >= 0)
        close(fd);
    uint32_t blocks = 0, shift = 0;
    memcpy(&blocks, super + 4, 4);
    memcpy(&shift, super + 24, 4);
    valid = valid && blocks > 0 && shift <= 6 && (uint64_t)blocks * (1024ULL << shift) <= (uint64_t)st.st_size;
    return valid || fail(error, @"镜像必须是完整的 ext4 文件；Android sparse 镜像需要先转换。");
}
static NSString *sha256(NSURL *url, NSError **error)
{
    if (!regular(url)) {
        fail(error, @"镜像文件缺失或为符号链接。");
        return nil;
    }
    NSFileHandle *file = [NSFileHandle fileHandleForReadingFromURL:url error:error];
    if (!file)
        return nil;
    CC_SHA256_CTX ctx;
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    CC_SHA256_Init(&ctx);
    for (;;) {
        NSData *data = [file readDataUpToLength:1024 * 1024 error:error];
        if (!data) {
            [file closeFile];
            return nil;
        }
        if (!data.length)
            break;
        CC_SHA256_Update(&ctx, data.bytes, (CC_LONG)data.length);
    }
    unsigned char bytes[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256_Final(bytes, &ctx);
#pragma clang diagnostic pop
    [file closeFile];
    NSMutableString *digest = [NSMutableString new];
    for (unsigned char byte : bytes)
        [digest appendFormat:@"%02x", byte];
    return digest;
}

@implementation NceDeviceStore {
    NSURL *_resources;
    NSFileManager *_files;
    int _catalogLock;
}
- (instancetype)initWithRoot:(NSURL *)root resources:(NSURL *)resources
{
    if ((self = [super init])) {
        _catalogLock = -1;
        _root = root;
        _resources = resources;
        _files = [NSFileManager new];
    }
    return self;
}
- (void)dealloc
{
    if (_catalogLock >= 0)
        close(_catalogLock);
}
- (NSURL *)devicesURL
{
    return [_root URLByAppendingPathComponent:@"Devices" isDirectory:YES];
}
- (BOOL)writeRecord:(NSDictionary *)record directory:(NSURL *)directory error:(NSError **)error
{
    NSData *data = [NSJSONSerialization dataWithJSONObject:record options:NSJSONWritingPrettyPrinted error:error];
    return data && [data writeToURL:[directory URLByAppendingPathComponent:@"device.json"] options:NSDataWritingAtomic error:error];
}
+ (BOOL)validateDeviceAtDirectory:(NSURL *)directory resources:(NSURL *)resources runtimeResources:(NSURL *)runtimeResources error:(NSError **)error
{
    NSURL *recordURL = [directory URLByAppendingPathComponent:@"device.json"];
    BOOL bundled = [resources.URLByResolvingSymlinksInPath.path isEqual:runtimeResources.URLByResolvingSymlinksInPath.path];
    struct stat st;
    // 独立 CLI 使用包内资源及新数据目录，无需先创建设备列表记录。
    if (lstat(recordURL.fileSystemRepresentation, &st) && errno == ENOENT && bundled)
        return YES;
    NSDictionary *record = json(recordURL, error);
    if (!record)
        return NO;
    if (![record[@"builtin"] isKindOfClass:NSNumber.class])
        return fail(error, @"设备记录损坏，请检查设备数据目录。");
    if ([record[@"builtin"] boolValue])
        return bundled || fail(error, @"内置设备需要使用当前应用的系统资源。");
    NSURL *images = [directory URLByAppendingPathComponent:@"Images"];
    if (![resources.URLByResolvingSymlinksInPath.path isEqual:images.URLByResolvingSymlinksInPath.path])
        return fail(error, @"导入设备的系统资源目录与设备记录不匹配。");
    NSString *identifier = record[@"runtime"];
    if (!validRuntime(identifier))
        return fail(error, @"此导入设备缺少运行组件版本，请重新导入匹配的镜像。原设备数据已保留。");
    NSDictionary *runtime = json([runtimeResources URLByAppendingPathComponent:@"Runtime.json"], error);
    if (!runtime)
        return NO;
    if (!validRuntime(runtime[@"runtime"]) || ![identifier isEqual:runtime[@"runtime"]])
        return fail(error, @"此导入设备与当前应用的运行组件版本不匹配。请使用匹配的应用版本或重新导入镜像，原设备数据已保留。");
    return YES;
}
- (NSArray<NceDevice *> *)load:(NSError **)error
{
    if (![_files createDirectoryAtURL:self.devicesURL withIntermediateDirectories:YES attributes:nil error:error])
        return nil;
    if (_catalogLock < 0) {
        int lock = open([_root URLByAppendingPathComponent:@"devices.lock"].fileSystemRepresentation, O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (lock < 0 || flock(lock, LOCK_EX | LOCK_NB)) {
            if (lock >= 0)
                close(lock);
            fail(error, @"设备列表已在另一个应用进程中打开。");
            return nil;
        }
        _catalogLock = lock;
        for (NSURL *entry in [_files contentsOfDirectoryAtURL:self.devicesURL includingPropertiesForKeys:nil options:0 error:error]) {
            if ([entry.lastPathComponent hasPrefix:@".import-"] && ![_files removeItemAtURL:entry error:error])
                return nil;
        }
        [_root setResourceValue:@YES forKey:NSURLIsExcludedFromBackupKey error:nil];
    }
    NSURL *marker = [_root URLByAppendingPathComponent:@"devices-initialized"];
    if (![_files fileExistsAtPath:marker.path]) {
        // The fixed default ID makes first-run initialization repeatable after an interrupted write.
        NSURL *directory = [self.devicesURL URLByAppendingPathComponent:@"default"];
        if (![_files createDirectoryAtURL:directory withIntermediateDirectories:YES attributes:nil error:error])
            return nil;
        if (![self writeRecord:@{@"id" : @"default", @"name" : @"Android", @"image" : @"内置 Android 16", @"builtin" : @YES} directory:directory error:error] ||
            ![[NSData data] writeToURL:marker options:NSDataWritingAtomic error:error])
            return nil;
    }
    NSArray *entries = [_files contentsOfDirectoryAtURL:self.devicesURL includingPropertiesForKeys:nil options:NSDirectoryEnumerationSkipsHiddenFiles error:error];
    if (!entries)
        return nil;
    NSMutableArray *devices = [NSMutableArray new];
    for (NSURL *directory in entries) {
        struct stat st;
        if (lstat(directory.fileSystemRepresentation, &st) || !S_ISDIR(st.st_mode))
            continue;
        NSDictionary *record = json([directory URLByAppendingPathComponent:@"device.json"], error);
        if (!record || ![record[@"id"] isEqual:directory.lastPathComponent] ||
            ![record[@"name"] isKindOfClass:NSString.class] || ![record[@"image"] isKindOfClass:NSString.class] ||
            ![record[@"builtin"] isKindOfClass:NSNumber.class]) {
            fail(error, @"设备记录损坏，请检查设备数据目录。");
            return nil;
        }
        [devices addObject:[[NceDevice alloc] initWithRecord:record directory:directory resources:_resources]];
    }
    [devices sortUsingComparator:^NSComparisonResult(NceDevice *a, NceDevice *b) {
      return [a.name localizedStandardCompare:b.name];
    }];
    return devices;
}
- (NceDevice *)createBuiltinNamed:(NSString *)name error:(NSError **)error
{
    name = [name stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
    if (!name.length || name.length > 128) {
        fail(error, @"名称需要包含 1 到 128 个字符。");
        return nil;
    }
    NSString *identifier = NSUUID.UUID.UUIDString;
    NSURL *directory = [self.devicesURL URLByAppendingPathComponent:identifier];
    NSDictionary *record = @{@"id" : identifier, @"name" : name, @"image" : @"内置 Android 16", @"builtin" : @YES};
    if (![_files createDirectoryAtURL:directory withIntermediateDirectories:YES attributes:nil error:error])
        return nil;
    if (![self writeRecord:record directory:directory error:error]) {
        [_files removeItemAtURL:directory error:nil];
        return nil;
    }
    return [[NceDevice alloc] initWithRecord:record directory:directory resources:_resources];
}
- (NceDevice *)importImage:(NSURL *)source raw:(BOOL)raw error:(NSError **)error
{
    NSDictionary *manifest = raw ? nil : json([source URLByAppendingPathComponent:@"manifest.json"], error);
    NSDictionary *runtime = json([_resources URLByAppendingPathComponent:@"Runtime.json"], error);
    if (!runtime)
        return nil;
    if (!validRuntime(runtime[@"runtime"])) {
        fail(error, @"当前应用缺少有效的运行组件版本，无法导入镜像。");
        return nil;
    }
    if (!raw && (!manifest || ![manifest[@"format"] isEqual:@1] ||
                 ![manifest[@"runtime"] isEqual:runtime[@"runtime"]] ||
                 ![manifest[@"name"] isKindOfClass:NSString.class] || ![manifest[@"files"] isKindOfClass:NSDictionary.class])) {
        fail(error, @"镜像包格式或运行组件版本不匹配。");
        return nil;
    }
    NSString *identifier = NSUUID.UUID.UUIDString;
    NSURL *temporary = [self.devicesURL URLByAppendingPathComponent:[@".import-" stringByAppendingString:identifier]];
    NSURL *images = [temporary URLByAppendingPathComponent:@"Images"];
    if (![_files createDirectoryAtURL:images withIntermediateDirectories:YES attributes:nil error:error])
        return nil;
    BOOL success = NO;
    NceDevice *device = nil;
    @try {
        NSMutableDictionary *sizes = [NSMutableDictionary new];
        for (NSString *name in @[ @"system.img", @"android-data-seed.img", @"lkl-image.bin" ]) {
            BOOL kernel = [name isEqual:@"lkl-image.bin"];
            NSURL *input = kernel || (raw && ![name isEqual:@"system.img"]) ? [_resources URLByAppendingPathComponent:name] : raw ? source
                                                                                                                                  : [source URLByAppendingPathComponent:name];
            NSURL *output = [images URLByAppendingPathComponent:name];
            if (!regular(input) || ![_files copyItemAtURL:input toURL:output error:error]) {
                fail(error, @"镜像文件无法复制，请检查文件及可用空间。");
                return nil;
            }
            if (!kernel && !ext4(output, error))
                return nil;
            sizes[name] = [_files attributesOfItemAtPath:output.path error:error][NSFileSize];
            if (!raw && !kernel) {
                id expected = manifest[@"files"][name];
                if (![expected isKindOfClass:NSString.class] || ![sha256(output, error) isEqual:expected]) {
                    fail(error, @"镜像校验失败，文件可能损坏。");
                    return nil;
                }
            }
        }
        NSData *sizeData = [NSPropertyListSerialization dataWithPropertyList:sizes format:NSPropertyListXMLFormat_v1_0 options:0 error:error];
        if (![sizeData writeToURL:[images URLByAppendingPathComponent:@"ResourceSizes.plist"] options:NSDataWritingAtomic error:error])
            return nil;
        NSString *name = raw ? source.lastPathComponent.stringByDeletingPathExtension : manifest[@"name"];
        if (!name.length || name.length > 128)
            name = @"Android";
        NSDictionary *record = @{@"id" : identifier, @"name" : name, @"image" : raw ? @"导入的 system.img" : name, @"builtin" : @NO, @"runtime" : runtime[@"runtime"]};
        if (![self writeRecord:record directory:temporary error:error])
            return nil;
        NSURL *directory = [self.devicesURL URLByAppendingPathComponent:identifier];
        if (![_files moveItemAtURL:temporary toURL:directory error:error])
            return nil;
        success = YES;
        device = [[NceDevice alloc] initWithRecord:record directory:directory resources:_resources];
    } @finally {
        if (!success)
            [_files removeItemAtURL:temporary error:nil];
    }
    return device;
}
- (BOOL)deleteDevice:(NceDevice *)device error:(NSError **)error
{
    if (![device.directory.URLByDeletingLastPathComponent.URLByResolvingSymlinksInPath.path isEqual:self.devicesURL.URLByResolvingSymlinksInPath.path])
        return fail(error, @"设备目录无效。");
    int lock = open([device.directory URLByAppendingPathComponent:@"session.lock"].fileSystemRepresentation, O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (lock < 0 || flock(lock, LOCK_EX | LOCK_NB)) {
        if (lock >= 0)
            close(lock);
        return fail(error, @"设备仍在运行，请先停止。");
    }
    BOOL result = [_files removeItemAtURL:device.directory error:error];
    close(lock);
    return result;
}
- (BOOL)renameDevice:(NceDevice *)device name:(NSString *)name error:(NSError **)error
{
    NSString *trimmed = [name stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
    if (!trimmed.length || trimmed.length > 128)
        return fail(error, @"名称需要包含 1 到 128 个字符。");
    if (![device.directory.URLByDeletingLastPathComponent.URLByResolvingSymlinksInPath.path isEqual:self.devicesURL.URLByResolvingSymlinksInPath.path])
        return fail(error, @"设备目录无效。");
    NSDictionary *record = json([device.directory URLByAppendingPathComponent:@"device.json"], error);
    if (!record || ![record[@"id"] isEqual:device.identifier])
        return fail(error, @"设备记录无效。");
    NSMutableDictionary *updated = [record mutableCopy];
    updated[@"name"] = trimmed;
    return [self writeRecord:updated directory:device.directory error:error];
}
@end
