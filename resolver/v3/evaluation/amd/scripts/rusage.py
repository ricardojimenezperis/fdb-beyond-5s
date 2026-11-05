import os, subprocess, sys, time
t0 = time.monotonic()
p = subprocess.Popen(sys.argv[2:])
_, status, ru = os.wait4(p.pid, 0)
t1 = time.monotonic()
code = os.waitstatus_to_exitcode(status)
with open(sys.argv[1], "w") as f:
    f.write("exit=%d wall_s=%.3f maxrss_kib=%d utime_s=%.3f stime_s=%.3f minflt=%d majflt=%d nvcsw=%d nivcsw=%d\n" % (
        code, t1 - t0, ru.ru_maxrss, ru.ru_utime, ru.ru_stime, ru.ru_minflt, ru.ru_majflt, ru.ru_nvcsw, ru.ru_nivcsw))
sys.exit(code)
