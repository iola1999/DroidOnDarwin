/* SPDX-License-Identifier: GPL-2.0-or-later */
#import "Storage.h"
#include <assert.h>
#include <fcntl.h>
#include <copyfile.h>
#include <dlfcn.h>
#include <unistd.h>

static BOOL simulateFullDisk;
extern "C" int copyfile(const char *from, const char *to, copyfile_state_t state, copyfile_flags_t flags)
{
    if (simulateFullDisk) {
        errno = ENOSPC;
        return -1;
    }
    auto original = (int (*)(const char *, const char *, copyfile_state_t, copyfile_flags_t))dlsym(RTLD_NEXT, "copyfile");
    assert(original);
    return original(from, to, state, flags);
}

static void writeFixture(NSURL *path, BOOL ext4)
{
    NSMutableData *data = [NSMutableData dataWithLength:4096];
    if (ext4) {
        unsigned char *bytes = (unsigned char *)data.mutableBytes;
        bytes[1024 + 0x38] = 0x53;
        bytes[1024 + 0x39] = 0xef;
    }
    assert([data writeToURL:path atomically:YES]);
}

int main(void)
{
    @autoreleasepool {
        NSFileManager *files = NSFileManager.defaultManager;
        NSURL *root = [NSURL fileURLWithPath:[NSTemporaryDirectory() stringByAppendingPathComponent:NSUUID.UUID.UUIDString]];
        NSURL *resources = [root URLByAppendingPathComponent:@"Resources"];
        NSURL *data = [root URLByAppendingPathComponent:@"Data"];
        assert([files createDirectoryAtURL:resources withIntermediateDirectories:YES attributes:nil error:nil]);
        NSError *error = nil;
        NceStorage *missing = [NceStorage new];
        assert(![missing prepareResources:resources directory:data error:&error]);
        assert(![files fileExistsAtPath:data.path]);
        for (NSString *name in @[ @"lkl-image.bin", @"system.img", @"android-data-seed.img" ])
            writeFixture([resources URLByAppendingPathComponent:name], YES);
        NSDictionary *sizes = @{@"lkl-image.bin" : @4096,
                                @"system.img" : @4096,
                                @"android-data-seed.img" : @4096};
        assert([sizes writeToURL:[resources URLByAppendingPathComponent:@"ResourceSizes.plist"] atomically:YES]);
        simulateFullDisk = YES;
        NceStorage *fullDisk = [NceStorage new];
        assert(![fullDisk prepareResources:resources directory:data error:&error]);
        assert(error.code == ENOSPC);
        assert(![files fileExistsAtPath:[data URLByAppendingPathComponent:@"android-data.img"].path]);
        fullDisk = nil;
        simulateFullDisk = NO;
        assert([files createDirectoryAtURL:data withIntermediateDirectories:YES attributes:nil error:nil]);
        NSURL *partial = [data URLByAppendingPathComponent:@"android-data.creating"];
        assert([[@"partial" dataUsingEncoding:NSUTF8StringEncoding] writeToURL:partial atomically:YES]);
        NceStorage *first = [NceStorage new];
        assert([first prepareResources:resources directory:data error:&error]);
        assert(![files fileExistsAtPath:partial.path]);
        assert([[NSData dataWithContentsOfURL:first.disk] isEqual:[NSData dataWithContentsOfURL:[resources URLByAppendingPathComponent:@"android-data-seed.img"]]]);
        NceStorage *second = [NceStorage new];
        assert(![second prepareResources:resources directory:data error:&error]);
        assert(error.code == EWOULDBLOCK);
        second = nil;
        NSURL *disk = first.disk;
        int fd = open(disk.fileSystemRepresentation, O_RDWR);
        assert(fd >= 0 && pwrite(fd, "saved", 5, 3000) == 5);
        close(fd);
        first = nil;
        NceStorage *restarted = [NceStorage new];
        assert([restarted prepareResources:resources directory:data error:&error]);
        char value[5];
        fd = open(disk.fileSystemRepresentation, O_RDONLY);
        assert(fd >= 0 && pread(fd, value, 5, 3000) == 5 && !memcmp(value, "saved", 5));
        close(fd);
        restarted = nil;
        NSData *persisted = [NSData dataWithContentsOfURL:disk];
        assert([files removeItemAtURL:disk error:nil]);
        assert([files createSymbolicLinkAtURL:disk withDestinationURL:[resources URLByAppendingPathComponent:@"android-data-seed.img"] error:nil]);
        NceStorage *linked = [NceStorage new];
        assert(![linked prepareResources:resources directory:data error:&error]);
        linked = nil;
        assert([files removeItemAtURL:disk error:nil]);
        writeFixture(disk, NO);
        NceStorage *corrupt = [NceStorage new];
        assert(![corrupt prepareResources:resources directory:data error:&error]);
        assert([[NSData dataWithContentsOfURL:disk] length] == 4096);
        corrupt = nil;
        assert([persisted writeToURL:disk atomically:YES]);
        NceStorage *recovered = [NceStorage new];
        assert([recovered prepareResources:resources directory:data error:&error]);
        recovered = nil;
        assert([files removeItemAtURL:root error:nil]);
        puts("storage checks passed: missing resources, interrupted creation, exclusive lock, persistent data, simulated ENOSPC, symlink and corrupt image");
    }
}
