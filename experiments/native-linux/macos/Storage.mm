/* SPDX-License-Identifier: GPL-2.0-or-later */
#import "Storage.h"
#include <copyfile.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

@implementation NceStorage {
    int _lock;
}

- (instancetype)init
{
    if ((self = [super init]))
        _lock = -1;
    return self;
}

- (BOOL)fail:(NSString *)message code:(NSInteger)code error:(NSError **)error
{
    if (error)
        *error = [NSError errorWithDomain:@"app.droidondarwin.storage"
                                     code:code
                                 userInfo:@{NSLocalizedDescriptionKey : message}];
    return NO;
}

- (BOOL)prepareResources:(NSURL *)resources directory:(NSURL *)directory error:(NSError **)error
{
    NSFileManager *files = NSFileManager.defaultManager;
    NSDictionary *sizes = [NSDictionary dictionaryWithContentsOfURL:[resources URLByAppendingPathComponent:@"ResourceSizes.plist"]];
    for (NSString *name in @[ @"lkl-image.bin", @"system.img", @"android-data-seed.img" ]) {
        NSURL *resource = [resources URLByAppendingPathComponent:name];
        NSDictionary *attributes = [files attributesOfItemAtPath:resource.path error:error];
        if (![attributes[NSFileType] isEqual:NSFileTypeRegular] || [sizes[name] unsignedLongLongValue] < 2048 ||
            ![attributes[NSFileSize] isEqual:sizes[name]])
            return [self fail:[@"安装包缺少或损坏文件：" stringByAppendingString:name] code:EINVAL error:error];
    }
    if (![files createDirectoryAtURL:directory
            withIntermediateDirectories:YES
                             attributes:@{NSFilePosixPermissions : @0700}
                                  error:error])
        return NO;
    _directory = directory;
    NSURL *lockURL = [directory URLByAppendingPathComponent:@"session.lock"];
    _lock = open(lockURL.fileSystemRepresentation, O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (_lock < 0)
        return [self fail:@"无法打开数据目录的占用记录。" code:errno error:error];
    if (flock(_lock, LOCK_EX | LOCK_NB))
        return [self fail:@"此数据目录正在使用，请先退出另一个 Android 窗口。" code:errno error:error];

    _disk = [directory URLByAppendingPathComponent:@"android-data.img"];
    if (![files fileExistsAtPath:_disk.path]) {
        NSURL *seed = [resources URLByAppendingPathComponent:@"android-data-seed.img"];
        NSURL *temporary = [directory URLByAppendingPathComponent:@"android-data.creating"];
        // 首次复制中断会留下临时文件；持有目录锁后才能清理并重试。
        if ([files fileExistsAtPath:temporary.path] && ![files removeItemAtURL:temporary error:error])
            return NO;
        if (copyfile(seed.fileSystemRepresentation, temporary.fileSystemRepresentation, NULL,
                     COPYFILE_DATA | COPYFILE_CLONE | COPYFILE_EXCL))
            return [self fail:@"无法创建 Android 数据盘，请检查可用空间和目录权限。" code:errno error:error];
        int fd = open(temporary.fileSystemRepresentation, O_RDWR | O_CLOEXEC);
        if (fd < 0)
            return [self fail:@"无法打开新建的数据盘。" code:errno error:error];
        int result = fsync(fd);
        int saved = errno;
        close(fd);
        if (result)
            return [self fail:@"无法保存 Android 数据盘。" code:saved error:error];
        if (![files moveItemAtURL:temporary toURL:_disk error:error])
            return NO;
    }
    int fd = open(_disk.fileSystemRepresentation, O_RDWR | O_CLOEXEC | O_NOFOLLOW);
    unsigned char magic[2] = {};
    struct stat attributes = {};
    BOOL valid = fd >= 0 && fstat(fd, &attributes) == 0 && S_ISREG(attributes.st_mode) &&
                 pread(fd, magic, sizeof(magic), 1024 + 0x38) == sizeof(magic) &&
                 magic[0] == 0x53 && magic[1] == 0xef;
    if (fd >= 0)
        close(fd);
    if (!valid)
        return [self fail:@"Android 数据盘无法读写或格式损坏。请检查数据目录中的 android-data.img。" code:EINVAL error:error];
    NSURL *logs = [directory URLByAppendingPathComponent:@"Logs" isDirectory:YES];
    if (![files createDirectoryAtURL:logs withIntermediateDirectories:YES attributes:nil error:error])
        return NO;
    _log = [logs URLByAppendingPathComponent:[NSString stringWithFormat:@"session-%@.log", NSUUID.UUID.UUIDString]];
    return YES;
}

- (void)dealloc
{
    if (_lock >= 0)
        close(_lock);
}
@end
