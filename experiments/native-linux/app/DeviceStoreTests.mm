/* SPDX-License-Identifier: GPL-2.0-or-later */
#import "DeviceStore.h"
#import "../macos/Storage.h"
#include <CommonCrypto/CommonDigest.h>
#include <assert.h>
#include <fcntl.h>
#include <unistd.h>

static void writeJSON(NSURL *url, NSDictionary *record)
{
    assert([[NSJSONSerialization dataWithJSONObject:record options:0 error:nil] writeToURL:url atomically:YES]);
}
static NSMutableDictionary *readJSON(NSURL *url)
{
    return [NSJSONSerialization JSONObjectWithData:[NSData dataWithContentsOfURL:url] options:NSJSONReadingMutableContainers error:nil];
}
int main(void)
{
    @autoreleasepool {
        NSFileManager *files = NSFileManager.defaultManager;
        NSURL *root = [NSURL fileURLWithPath:[NSTemporaryDirectory() stringByAppendingPathComponent:NSUUID.UUID.UUIDString]];
        NSURL *resources = [root URLByAppendingPathComponent:@"Resources"], *data = [root URLByAppendingPathComponent:@"Data"];
        assert([files createDirectoryAtURL:resources withIntermediateDirectories:YES attributes:nil error:nil]);
        assert([files createDirectoryAtURL:data withIntermediateDirectories:YES attributes:nil error:nil]);
        NSMutableData *bytes = [NSMutableData dataWithLength:4096];
        unsigned char *raw = (unsigned char *)bytes.mutableBytes;
        raw[1028] = 4;
        raw[1080] = 0x53;
        raw[1081] = 0xef;
        NSMutableDictionary *sizes = [NSMutableDictionary new];
        for (NSString *name in @[ @"system.img", @"lkl-image.bin", @"android-data-seed.img" ]) {
            assert([bytes writeToURL:[resources URLByAppendingPathComponent:name] atomically:YES]);
            sizes[name] = @4096;
        }
        assert([sizes writeToURL:[resources URLByAppendingPathComponent:@"ResourceSizes.plist"] atomically:YES]);
        writeJSON([resources URLByAppendingPathComponent:@"Runtime.json"], @{@"runtime" : @"test-runtime"});
        NSURL *standalone = [root URLByAppendingPathComponent:@"Standalone"];
        assert([NceDeviceStore validateDeviceAtDirectory:standalone resources:resources runtimeResources:resources error:nil]);
        assert(![files fileExistsAtPath:standalone.path]);
        assert(![NceDeviceStore validateDeviceAtDirectory:standalone resources:data runtimeResources:resources error:nil]);
        NceDeviceStore *store = [[NceDeviceStore alloc] initWithRoot:data resources:resources];
        NSError *error = nil;
        NSArray<NceDevice *> *devices = [store load:&error];
        assert(devices.count == 1 && !error);
        NceDeviceStore *duplicate = [[NceDeviceStore alloc] initWithRoot:data resources:resources];
        assert(![duplicate load:&error]);
        duplicate = nil;
        NceDevice *first = devices[0];
        assert(![files fileExistsAtPath:[first.directory URLByAppendingPathComponent:@"android-data.img"].path]);
        NceDevice *second = [store createBuiltinNamed:@"Android" error:&error];
        assert(second && [store load:&error].count == 2 && ![first.identifier isEqual:second.identifier]);
        assert(![store renameDevice:second name:@"  " error:&error]);
        assert([store renameDevice:second name:@"  My Android  " error:&error]);
        NSArray<NceDevice *> *renamed = [store load:&error];
        assert([renamed[1].name isEqual:@"My Android"]);
        assert([renamed[1].identifier isEqual:second.identifier]);
        NceStorage *session = [NceStorage new];
        assert([session prepareResources:first.resources directory:first.directory error:&error]);
        assert([[NSData dataWithContentsOfURL:session.disk] isEqual:bytes]);
        assert(![store deleteDevice:first error:&error]);
        session = nil;
        BOOL deleted = [store deleteDevice:first error:&error];
        if (!deleted)
            NSLog(@"delete: %@", error);
        assert(deleted);
        NSURL *package = [root URLByAppendingPathComponent:@"Android.droidimage"];
        assert([files createDirectoryAtURL:package withIntermediateDirectories:YES attributes:nil error:nil]);
        for (NSString *name in @[ @"system.img", @"android-data-seed.img" ])
            assert([bytes writeToURL:[package URLByAppendingPathComponent:name] atomically:YES]);
        unsigned char hash[32];
        CC_SHA256(bytes.bytes, (CC_LONG)bytes.length, hash);
        NSMutableString *digest = [NSMutableString new];
        for (unsigned char byte : hash)
            [digest appendFormat:@"%02x", byte];
        NSMutableDictionary *manifest = [@{@"format" : @1,
                                           @"name" : @"Imported",
                                           @"runtime" : @"wrong",
                                           @"files" : @{@"system.img" : digest, @"android-data-seed.img" : digest}} mutableCopy];
        NSURL *config = [package URLByAppendingPathComponent:@"manifest.json"];
        writeJSON(config, manifest);
        assert(![store importImage:package raw:NO error:&error]);
        manifest[@"runtime"] = @"test-runtime";
        writeJSON(config, manifest);
        NSURL *system = [package URLByAppendingPathComponent:@"system.img"];
        assert([files removeItemAtURL:system error:nil]);
        assert(![store importImage:package raw:NO error:&error]);
        assert([files createSymbolicLinkAtURL:system withDestinationURL:[resources URLByAppendingPathComponent:@"system.img"] error:nil]);
        assert(![store importImage:package raw:NO error:&error]);
        assert([files removeItemAtURL:system error:nil]);
        assert([bytes writeToURL:system atomically:YES]);
        NceDevice *imported = [store importImage:package raw:NO error:&error];
        assert(imported && !imported.builtin && [store load:&error].count == 2);
        NSURL *importRecord = [imported.directory URLByAppendingPathComponent:@"device.json"];
        assert([readJSON(importRecord)[@"runtime"] isEqual:@"test-runtime"]);
        assert([NceDeviceStore validateDeviceAtDirectory:imported.directory resources:imported.resources runtimeResources:resources error:&error]);
        session = [NceStorage new];
        assert([session prepareResources:imported.resources directory:imported.directory error:&error]);
        NSURL *importData = session.disk;
        NSData *originalData = [NSData dataWithContentsOfURL:importData];
        session = nil;
        int fd = open([package URLByAppendingPathComponent:@"system.img"].fileSystemRepresentation, O_WRONLY);
        assert(fd >= 0 && pwrite(fd, "corrupt", 7, 3000) == 7);
        close(fd);
        assert(![store importImage:package raw:NO error:&error]);
        assert([store load:&error].count == 2);
        NSURL *image = [resources URLByAppendingPathComponent:@"system.img"];
        NceDevice *rawImport = [store importImage:image raw:YES error:&error];
        assert(rawImport);
        NSURL *rawRecord = [rawImport.directory URLByAppendingPathComponent:@"device.json"];
        assert([readJSON(rawRecord)[@"runtime"] isEqual:@"test-runtime"]);
        assert([NceDeviceStore validateDeviceAtDirectory:rawImport.directory resources:rawImport.resources runtimeResources:resources error:&error]);

        NSURL *runtime = [resources URLByAppendingPathComponent:@"Runtime.json"];
        writeJSON(runtime, @{@"runtime" : @"new-incompatible-runtime"});
        for (NceDevice *device in @[ imported, rawImport ]) {
            error = nil;
            assert(![NceDeviceStore validateDeviceAtDirectory:device.directory resources:device.resources runtimeResources:resources error:&error]);
            assert([error.localizedDescription containsString:@"不匹配"]);
        }
        assert([store load:&error].count == 3);
        assert([store renameDevice:imported name:@"Preserved import" error:&error]);
        assert([readJSON(importRecord)[@"runtime"] isEqual:@"test-runtime"]);
        assert([NceDeviceStore validateDeviceAtDirectory:second.directory resources:second.resources runtimeResources:resources error:&error]);
        assert([[NSData dataWithContentsOfURL:importData] isEqual:originalData]);
        assert(![files fileExistsAtPath:[rawImport.directory URLByAppendingPathComponent:@"android-data.img"].path]);

        writeJSON(runtime, @{@"runtime" : @"test-runtime"});
        assert([NceDeviceStore validateDeviceAtDirectory:imported.directory resources:imported.resources runtimeResources:resources error:&error]);
        NSMutableDictionary *oldRecord = readJSON(importRecord);
        for (id identifier in @[ NSNull.null, @"", @" \n", @42 ]) {
            if (identifier == NSNull.null)
                [oldRecord removeObjectForKey:@"runtime"];
            else
                oldRecord[@"runtime"] = identifier;
            writeJSON(importRecord, oldRecord);
            error = nil;
            assert(![NceDeviceStore validateDeviceAtDirectory:imported.directory resources:imported.resources runtimeResources:resources error:&error]);
            assert([error.localizedDescription containsString:@"缺少运行组件版本"]);
            assert([store load:&error].count == 3);
        }
        oldRecord[@"runtime"] = @"test-runtime";
        writeJSON(importRecord, oldRecord);
        assert(![NceDeviceStore validateDeviceAtDirectory:imported.directory resources:resources runtimeResources:resources error:nil]);
        assert(![NceDeviceStore validateDeviceAtDirectory:second.directory resources:imported.resources runtimeResources:resources error:nil]);
        for (id identifier in @[ NSNull.null, @"", @" \n", @42 ]) {
            writeJSON(runtime, @{@"runtime" : identifier});
            assert(![NceDeviceStore validateDeviceAtDirectory:imported.directory resources:imported.resources runtimeResources:resources error:&error]);
            assert(![store importImage:image raw:YES error:&error]);
            assert(![store importImage:package raw:NO error:&error]);
            assert([store load:&error].count == 3);
        }
        writeJSON(runtime, @{@"runtime" : @"test-runtime"});
        assert([NceDeviceStore validateDeviceAtDirectory:imported.directory resources:imported.resources runtimeResources:resources error:&error]);
        assert([[NSData dataWithContentsOfURL:importData] isEqual:originalData]);
        NSURL *bad = [root URLByAppendingPathComponent:@"bad.img"];
        assert([[@"not ext4" dataUsingEncoding:NSUTF8StringEncoding] writeToURL:bad atomically:YES]);
        assert(![store importImage:bad raw:YES error:&error]);
        [files removeItemAtURL:bad error:nil];
        assert([files createSymbolicLinkAtURL:bad withDestinationURL:image error:nil]);
        assert(![store importImage:bad raw:YES error:&error]);
        [files removeItemAtURL:bad error:nil];
        raw[1028] = 100;
        assert([bytes writeToURL:bad atomically:YES]);
        assert(![store importImage:bad raw:YES error:&error]);
        for (NSURL *entry in [files contentsOfDirectoryAtURL:[data URLByAppendingPathComponent:@"Devices"] includingPropertiesForKeys:nil options:0 error:nil])
            assert(![entry.lastPathComponent hasPrefix:@".import-"]);
        for (NceDevice *device in [store load:&error])
            assert([store deleteDevice:device error:&error]);
        assert([store load:&error].count == 0);
        assert([files removeItemAtURL:root error:nil]);
        puts("device store checks passed: first-run initialization, independent data, active deletion lock, package version/hash, runtime upgrade and legacy-record rejection, preserved import data, raw ext4, truncation, symlink, failed import cleanup, empty library persistence");
    }
}
