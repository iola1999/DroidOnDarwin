/* SPDX-License-Identifier: Apache-2.0 */

// 工具与 JNI 库使用同一份 libart 中的 Runtime、Thread 和锁状态。
int nce_art_dex2oat_main(int argc, char **argv);

int main(int argc, char **argv)
{
    return nce_art_dex2oat_main(argc, argv);
}
