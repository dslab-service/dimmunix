package dimmunixTests;

import java.lang.management.ManagementFactory;
import java.lang.management.MemoryMXBean;
import java.util.Vector;
import java.util.concurrent.CyclicBarrier;
import com.sun.management.OperatingSystemMXBean;

public class TestPerformance {
	static int duration;//how long do we run the test 
	static int nThreads;//number of threads
	static int nSharedLocks;//number of shared locks
	static int syncDelayInside;//delay inside a lock
	static int syncDelayOutside;//delay between locks
	static long peakMemConsumption = 0;

	static final boolean contention = true;
	static final boolean randomization = true;

	static void refreshStats() {
		MemoryMXBean memBean = ManagementFactory.getMemoryMXBean();
		long m = memBean.getHeapMemoryUsage().getUsed()+ memBean.getNonHeapMemoryUsage().getUsed();            
		if (m > peakMemConsumption)
			peakMemConsumption = m;
	}

	public static void main(String[] args) {
		duration = Integer.parseInt(args[0]);
		nThreads = Integer.parseInt(args[1]);
		nSharedLocks = Integer.parseInt(args[2]);
		syncDelayInside = Integer.parseInt(args[3]);
		syncDelayOutside = Integer.parseInt(args[4]);            

		PerfTestThread.sharedLocks = new Object[nSharedLocks];
		for (int i = 0; i < nSharedLocks; i++)
			PerfTestThread.sharedLocks[i] = new Object();

		Vector<PerfTestThread> threads = new Vector<PerfTestThread>();

		CyclicBarrier barrier = new CyclicBarrier(nThreads);

		for (int i = 0; i < nThreads; i++)
			threads.add(new PerfTestThread(barrier, syncDelayInside, syncDelayOutside));

		for (Thread t: threads)
			t.start();

		long t0 = System.nanoTime();
		final long timeout = t0+ ((long)1000)* 1000* 1000* duration;

		for (PerfTestThread t: threads)
			t.timeout = timeout;

		new Thread(new Runnable() {
			public void run() {
				while (System.nanoTime() < timeout) {
					try {
						Thread.sleep(10);
					} catch (InterruptedException e) {
					}
					refreshStats();
				}
			}
		}).start();

		for (Thread t: threads) {
			try {
				t.join();
			} 
			catch (Exception e) {
			}
		}

		long execTimeMsec = (System.nanoTime()- t0)/ 1000/ 1000; 

		long nSyncs = 0;
		long sumSleep = 0;
		for (PerfTestThread t: threads) {
			nSyncs += t.nSyncs;
			sumSleep += t.sumSleep;
		}

		float syncsPerMsec = ((float)nSyncs)/ execTimeMsec;
		OperatingSystemMXBean osBean = (OperatingSystemMXBean)sun.management.ManagementFactory.getOperatingSystemMXBean();
		float cpuSec = ((float)osBean.getProcessCpuTime())/ 1000/ 1000/ 1000;
		float memMB = ((float)(peakMemConsumption))/ 1024/ 1024;
		System.out.print(syncsPerMsec+ "\t"+ cpuSec+ "\t"+ memMB);
	}
}
