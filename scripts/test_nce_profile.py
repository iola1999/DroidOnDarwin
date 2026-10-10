#!/usr/bin/env python3
"""检查调用树的自身样本及映射调用与等待栈的分类边界。"""
import unittest
from analyze_nce_profile import art_summary, compiler_filter_summary, fast_baseline_summary, log_summary, sample_summary


class SampleTests(unittest.TestCase):
    def test_fast_baseline_counts_keep_thread_and_runtime_boundaries(self):
        result = fast_baseline_summary("""NCE_ART_FAST_BASELINE pid=10 epoch=50 tid=12 installed=4
NCE_ART_FAST_BASELINE pid=10 epoch=50 tid=12 installed=2
NCE_ART_FAST_BASELINE pid=10 epoch=50 tid=13 installed=8
NCE_ART_FAST_BASELINE pid=10 epoch=70 tid=12 installed=1
NCE_ART_FAST_BASELINE pid=20 epoch=50 tid=12 installed=16""")
        self.assertEqual(len(result), 4)
        self.assertEqual([r['installed_lower_bound'] for r in result], [4, 8, 1, 16])
        self.assertEqual(fast_baseline_summary('ordinary log'), [])


    def test_compiler_counts_separate_processes_and_snapshots(self):
        result = compiler_filter_summary('''I art: NCE_ART_FILTER pid=10 kind=accepted count=4
I art: NCE_ART_FILTER pid=10 kind=InvokeStaticOrDirect count=1 method=java.lang.String Foo.run()
I art: NCE_ART_FILTER pid=20 kind=accepted count=2
NCE_ART_FILTER_SUMMARY pid=10 kind=accepted count=7
I art: NCE_ART_FILTER pid=10 kind=accepted count=8
NCE_ART_FILTER_SUMMARY pid=20 kind=accepted count=3
''')
        by_key = {(r['pid'], r['kind']): r for r in result}
        self.assertEqual(by_key[10, 'accepted']['count'], 8)
        self.assertFalse(by_key[10, 'accepted']['complete_snapshot'])
        self.assertEqual(by_key[20, 'accepted']['count'], 3)
        self.assertTrue(by_key[20, 'accepted']['complete_snapshot'])
        self.assertEqual(by_key[10, 'InvokeStaticOrDirect']['examples'], ['java.lang.String Foo.run()'])
        self.assertEqual(compiler_filter_summary('ordinary log'), [])
        result = compiler_filter_summary('''NCE_ART_FILTER pid=1 epoch=100 kind=accepted count=4
NCE_ART_FILTER pid=1 epoch=200 kind=accepted count=1
''')
        self.assertEqual([(r['epoch'], r['count']) for r in result], [(100, 4), (200, 1)])

    def test_exclusive_samples_and_remap_mach_call(self):
        text = '''Call graph:
    10 Thread_1
    + 10 entry  (in host)
    +   7 nce_memory_remap  (in host)
    +   ! 6 mach_msg2_trap  (in kernel)
    +   3 prepare_resume  (in host)
    10 Thread_2
    + 10 event_loop  (in host)
    +   10 mach_msg2_trap  (in kernel)
Total number in stack:
'''
        result = sample_summary(text)
        self.assertEqual(result['all_thread_samples'], 20)
        self.assertEqual(result['nonwait_samples'], 10)
        self.assertEqual(result['groups'], {'host_mapping': 7, 'resume_entry': 3, 'explicit_wait': 10})

    def test_reject_invalid_tree(self):
        with self.assertRaises(ValueError):
            sample_summary('Call graph:\n    1 Thread_1\n    + 2 entry  (in host)\n')

    def test_component_timers_stay_separate(self):
        text = '''10-10 02:00:00.000 1 1 V SystemUIBootTiming: StartServices child took to complete: 30ms
10-10 02:00:00.001 1 1 V SystemUIBootTiming: StartServices took to complete: 40ms
{"profile":"host_mapping","elapsed_ms":100,"data_ms":5,"code_ms":2,"revoke_ms":3}
{"profile":"host_mapping","elapsed_ms":200,"data_ms":8,"code_ms":3,"revoke_ms":5}
'''
        result = log_summary(text)
        self.assertEqual([r['ms'] for r in result['component_timings']], [30, 40])
        self.assertEqual([r['mapping_ms'] for r in result['mapping_intervals']], [10, 6])

    def test_art_snapshot_process_and_thread_boundaries(self):
        text = '''----- pid 10 at 2026-10-10 00:00:00+0000 -----
Cmd line: com.android.systemui
"main" prio=5 tid=1 Native
  native: #00 pc 123 /system/lib64/libbinder.so (transact+4)
  at android.os.BinderProxy.transact(Native method)
"worker" prio=5 tid=2 Waiting
  at worker.run(Worker.java:1)
Total time spent in GC: 1.086s
----- end 10 -----
----- pid 20 at 2026-10-10 00:00:01+0000 -----
Cmd line: system_server
"main" prio=5 tid=1 Native
  at android.os.Looper.loop(Looper.java:1)
Total time spent in GC: 677.554ms
----- end 20 -----
'''
        result = art_summary(text)
        self.assertEqual([r['pid'] for r in result], [10, 20])
        self.assertEqual(result[0]['main_java_frames'], ['at android.os.BinderProxy.transact(Native method)'])
        self.assertEqual(result[0]['statistics'], ['Total time spent in GC: 1.086s'])

    def test_optional_mmu_reports_preserve_fault_categories(self):
        self.assertEqual(log_summary('ordinary log')['mmu_cumulative'], [])
        report = '{"profile":"linux_mmu","switches":4,"present_faults":8,"linux_faults":3,"fast_faults":12,"restored_pages":20}'
        result = log_summary(report)['mmu_cumulative'][0]
        self.assertEqual(result['present_faults'], 8)
        self.assertEqual(result['linux_faults'], 3)
        self.assertEqual(result['fast_faults'], 12)
        self.assertEqual(result['line'], 1)


if __name__ == '__main__':
    unittest.main()
