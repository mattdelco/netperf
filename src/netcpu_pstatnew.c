
/*
#  Copyright 2021 Hewlett Packard Enterprise Development LP
#
# Permission is hereby granted, free of charge, to any person obtaining a
# copy of this software and associated documentation files (the "Software"),
# to deal in the Software without restriction, including without limitation
# the rights to use, copy, modify, merge, publish, distribute, sublicense,
# and/or sell copies of the Software, and to permit persons to whom the
# Software is furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included in
# all copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
#
# IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
# DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
# OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
# USE OR OTHER DEALINGS IN THE SOFTWARE.

*/


char   netcpu_pstatnew_id[]="\
@(#)netcpu_pstatnew.c (c) Copyright 2005-2012 Hewlett-Packard Company, Version 2.6.0";

/* since we "know" that this interface is available only on 11.23 and
   later, and that 11.23 and later are strictly 64-bit kernels, we can
   arbitrarily set _PSTAT64 here and not have to worry about it up in
   the configure script and makefiles. raj 2005/09/06 */

#if HAVE_CONFIG_H
# include <config.h>
#endif

#include <stdio.h>

#if HAVE_INTTYPES_H
# include <inttypes.h>
#else
# if HAVE_STDINT_H
#  include <stdint.h>
# endif
#endif

#include <unistd.h>

#if HAVE_LIMITS_H
# include <limits.h>
#endif

#include <sys/dk.h>
#include <sys/pstat.h>

/* HP-UX 11.23 seems to have added three other cycle counters to the
   original psp_idlecycles - one for user, one for kernel and one for
   interrupt. so, we can now use those to calculate CPU utilization
   without requiring any calibration phase.  raj 2005-02-16 */

#ifndef PSTAT_IPCINFO
# error Sorry, pstat() CPU utilization on 10.0 and later only
#endif

typedef struct cpu_time_counters {
  uint64_t idle;
  uint64_t user;
  uint64_t kernel;
  uint64_t interrupt;
} cpu_time_counters_t;

uint64_t lib_iticksperclktick;

#include "netsh.h"
#include "netlib.h"

/* the lib_start_count and lib_end_count arrays hold the starting
   and ending values of whatever is counting when the system is
   idle. The rate at which this increments during a test is compared
   with a previous calibrarion to arrive at a CPU utilization
   percentage. raj 2005-01-26 */

static cpu_time_counters_t  starting_cpu_counters[MAXCPUS];
static cpu_time_counters_t  ending_cpu_counters[MAXCPUS];
static cpu_time_counters_t  delta_cpu_counters[MAXCPUS];

/* there can be more "processors" in the system than are actually
   online. so, we can either walk all the processors one at a time,
   which would be slow, or we can track not just lib_num_loc_cpu,
   which is the number of active "processors" but also the total
   number, and retrieve all of them at one shot and walk the list
   once, ignoring those that are offline.  we will ass-u-me there is
   no change to the number of processors online while we are running
   or there will be strange things happening to CPU utilization. raj
   2010-04-27 */

static long max_proc_count;

void
cpu_util_init(void)
{
  struct pst_dynamic psd;
  if (pstat_getdynamic((struct pst_dynamic *)&psd,
                       (size_t)sizeof(psd), (size_t)1, 0) != -1) {
    max_proc_count = psd.psd_max_proc_cnt;
  }
  else {
    /* we hope this never happens */
    max_proc_count = lib_num_loc_cpus;
  }

  return;
}

void
cpu_util_terminate(void)
{
  return;
}

int
get_cpu_method(void)
{
  return HP_IDLE_COUNTER;
}

void
get_cpu_counters(cpu_time_counters_t *res)
{
      /* get the idle sycle counter for each processor. now while on a
	 64-bit kernel the ".psc_hi" and ".psc_lo" fields are 64 bits,
	 only the bottom 32-bits are actually valid.  don't ask me
	 why, that is just the way it is.  soo, we shift the psc_hi
	 value by 32 bits and then just sum-in the psc_lo value.  raj
	 2005/09/06 */
      struct pst_processor *psp;

      /* to handle the cases of "processors" present but disabled, we
	 will have to allocate a buffer big enough for everyone and
	 then walk the entire list, pulling data for those which are
	 online, assuming the processors online have not changed in
	 the middle of the run. raj 2010-04-27 */
      psp = (struct pst_processor *)malloc(max_proc_count * sizeof(*psp));
      if (psp == NULL) {
        printf("malloc(%d) failed!\n", max_proc_count * sizeof(*psp));
        exit(1);
      }
      if (pstat_getprocessor(psp, sizeof(*psp), max_proc_count, 0) != -1) {
        int i,j;
	/* we use lib_iticksperclktick in our sanity checking. we
	   ass-u-me it is the same value for each CPU - famous last
	   words no doubt. raj 2005/09/06 */
	lib_iticksperclktick = psp[0].psp_iticksperclktick;
	i = j = 0;
	while ((i < lib_num_loc_cpus) && (j < max_proc_count)) {
	  if (psp[j].psp_processor_state == PSP_SPU_DISABLED) {
	    j++;
	    continue;
	  }
	  /* we know that psp[j] is online */
          res[i].idle = (((uint64_t)psp[j].psp_idlecycles.psc_hi << 32) +
			 psp[j].psp_idlecycles.psc_lo);
          if(debug) {
            fprintf(where,
                    "\tidle[%d] = 0x%"PRIx64" ",
                    i,
                    res[i].idle);
            fflush(where);
          }
          res[i].user = (((uint64_t)psp[j].psp_usercycles.psc_hi << 32) +
			 psp[j].psp_usercycles.psc_lo);
          if(debug) {
            fprintf(where,
                    "user[%d] = 0x%"PRIx64" ",
                    i,
                    res[i].user);
            fflush(where);
          }
          res[i].kernel = (((uint64_t)psp[j].psp_systemcycles.psc_hi << 32) +
			    psp[j].psp_systemcycles.psc_lo);
          if(debug) {
            fprintf(where,
                    "kern[%d] = 0x%"PRIx64" ",
                    i,
                    res[i].kernel);
            fflush(where);
          }
          res[i].interrupt = (((uint64_t)psp[j].psp_interruptcycles.psc_hi << 32) +
			      psp[j].psp_interruptcycles.psc_lo);
          if(debug) {
            fprintf(where,
                    "intr[%d] = 0x%"PRIx64"\n",
                    i,
                    res[i].interrupt);
            fflush(where);
          }
	  i++;
	  j++;
	}
        free(psp);
      }
}

/* calibrate_pstatnew
   there really isn't anything much to do here since we have all the
   counters and use their ratios for CPU util measurement. raj
   2005-02-16 */

float
calibrate_idle_rate(int iterations, int interval)
{
  return 0.0;
}

static void
print_cpu_time_counters(char *name, int instance, cpu_time_counters_t *counters)
{
  fprintf(where,
	  "%s[%d]:\n"
	  "\t idle %llu\n"
	  "\t user %llu\n"
	  "\t kernel %llu\n"
	  "\t interrupt %llu\n",
	  name,instance,
	  counters[instance].idle,
	  counters[instance].user,
	  counters[instance].kernel,
	  counters[instance].interrupt);
}

float
calc_cpu_util_internal(float elapsed_time)
{
  int i;

  uint64_t total_cpu_cycles;
  uint64_t sanity_cpu_cycles;

#ifndef USE_INTEGER_MATH
  double fraction_idle;
  double fraction_user;
  double fraction_kernel;
  double fraction_interrupt;
  double estimated_fraction_interrupt;
#else
  uint64_t fraction_idle;
  uint64_t fraction_user;
  uint64_t fraction_kernel;
  uint64_t fraction_interrupt;
  uint64_t estimated_fraction_interrupt;

#define CALC_PERCENT 100
#define CALC_TENTH_PERCENT 1000
#define CALC_HUNDREDTH_PERCENT 10000
#define CALC_THOUSANDTH_PERCENT 100000
#define CALC_ACCURACY CALC_THOUSANDTH_PERCENT

#endif /* USE_INTEGER_MATH */
  float actual_rate;
  float correction_factor;

  memset(&lib_local_cpu_stats, 0, sizeof(lib_local_cpu_stats));

  /* It is possible that the library measured a time other than */
  /* the one that the user want for the cpu utilization */
  /* calculations - for example, tests that were ended by */
  /* watchdog timers such as the udp stream test. We let these */
  /* tests tell up what the elapsed time should be. */

  if (elapsed_time != 0.0) {
    correction_factor = (float) 1.0 +
      ((lib_elapsed - elapsed_time) / elapsed_time);
  }
  else {
    correction_factor = (float) 1.0;
  }

  /* calculate our sanity check on cycles */
  if (debug) {
    fprintf(where,
	    "lib_elapsed %g _SC_CLK_TCK %d lib_iticksperclktick %"PRIu64"\n",
	    lib_elapsed,
	    sysconf(_SC_CLK_TCK),
	    lib_iticksperclktick);
  }

  /* Ok, elsewhere I may have said that HP-UX 11.23 does the "right"
     thing in measuring user, kernel, interrupt and idle all together
     instead of overlapping interrupt with the others like an OS that
     shall not be named.  However.... it seems there is a bug in the
     accounting for interrupt cycles, whereby the cycles do not get
     properly accounted.  The sum of user, kernel, interrupt and idle
     does not equal the clock rate multiplied by the elapsed time.
     Some cycles go missing.

     Since we see agreement between netperf and glance/vsar with the
     old "pstat" mechanism, we can presume that the accounting for
     idle cycles is sufficiently accurate.  So, while we will still do
     math with user, kernel and interrupt cycles, we will only
     caculate CPU utilization based on the ratio of idle to _real_
     total cycles.  I am told that a "future release" of HP-UX will
     fix the interupt cycle accounting.  raj 2005/09/14 */

  /* calculate what the sum of CPU cycles _SHOULD_ be */
  sanity_cpu_cycles = (uint64_t) ((double)lib_elapsed *
    (double) sysconf(_SC_CLK_TCK) * (double)lib_iticksperclktick);

  /* this looks just like the looper case. at least I think it */
  /* should :) raj 4/95 */
  for (i = 0; i < lib_num_loc_cpus; i++) {

    /* we ass-u-me that these counters will never wrap during a
       netperf run.  this may not be a particularly safe thing to
       do. raj 2005-01-28 */
    delta_cpu_counters[i].idle = ending_cpu_counters[i].idle -
      starting_cpu_counters[i].idle;
    delta_cpu_counters[i].user = ending_cpu_counters[i].user -
      starting_cpu_counters[i].user;
    delta_cpu_counters[i].kernel = ending_cpu_counters[i].kernel -
      starting_cpu_counters[i].kernel;
    delta_cpu_counters[i].interrupt = ending_cpu_counters[i].interrupt -
      starting_cpu_counters[i].interrupt;

    if (debug) {
      print_cpu_time_counters("delta_cpu_counters",i,delta_cpu_counters);
    }

    /* now get the sum, which we ass-u-me does not overflow a 64-bit
       counter. raj 2005-02-16 */
    total_cpu_cycles =
      delta_cpu_counters[i].idle +
      delta_cpu_counters[i].user +
      delta_cpu_counters[i].kernel +
      delta_cpu_counters[i].interrupt;

    if (debug) {
      fprintf(where,
	      "total_cpu_cycles %"PRIu64" sanity_cpu_cycles %"PRIu64
	      " missing %"PRIu64"\n",
	      total_cpu_cycles,
	      sanity_cpu_cycles,
	      sanity_cpu_cycles - total_cpu_cycles);
    }

    /* since HP-UX 11.23 does the _RIGHT_ thing and idle/user/kernel
       does _NOT_ overlap with interrupt, we do not have to apply any
       correction kludge. raj 2005-02-16 */

#ifndef USE_INTEGER_MATH
    /* when the accounting for interrupt time gets its act together,
       we can use total_cpu_cycles rather than sanity_cpu_cycles, but
       until then, use sanity_cpu_ccles. raj 2005/09/14 */

    fraction_idle = (double)delta_cpu_counters[i].idle /
      (double)sanity_cpu_cycles;

    fraction_user = (double)delta_cpu_counters[i].user /
      (double)sanity_cpu_cycles;

    fraction_kernel = (double) delta_cpu_counters[i].kernel /
      (double)sanity_cpu_cycles;

    fraction_interrupt = (double)delta_cpu_counters[i].interrupt /
      (double)sanity_cpu_cycles;

    /* ass-u-me that it is only interrupt that is bogus, and assign
       all the "missing" cycles to it. raj 2005/09/14 */
    estimated_fraction_interrupt = ((double)delta_cpu_counters[i].interrupt +
				    (sanity_cpu_cycles - total_cpu_cycles)) /
      (double)sanity_cpu_cycles;

    if (debug) {
      fprintf(where,
	      "\tfraction_idle %g\n"
	      "\tfraction_user %g\n"
	      "\tfraction_kernel %g\n"
	      "\tfraction_interrupt %g WARNING, possibly under-counted!\n"
	      "\testimated_fraction_interrupt %g\n",
	      fraction_idle,
	      fraction_user,
	      fraction_kernel,
	      fraction_interrupt,
	      estimated_fraction_interrupt);
    }

    /* and finally, what is our CPU utilization? */
    lib_local_per_cpu_util[i] = 100.0 - (fraction_idle * 100.0);
#else
    /* and now some fun with integer math.  i initially tried to
       promote things to long doubled but that didn't seem to result
       in happiness and joy. raj 2005-01-28 */

    /* multiply by 100 and divide by total and you get whole
       percentages. multiply by 1000 and divide by total and you get
       tenths of percentages.  multiply by 10000 and divide by total
       and you get hundredths of percentages. etc etc etc raj
       2005-01-28 */

    /* when the accounting for interrupt time gets its act together,
       we can use total_cpu_cycles rather than sanity_cpu_cycles, but
       until then, use sanity_cpu_ccles. raj 2005/09/14 */

    fraction_idle =
      (delta_cpu_counters[i].idle * CALC_ACCURACY) / sanity_cpu_cycles;

    fraction_user =
      (delta_cpu_counters[i].user * CALC_ACCURACY) / sanity_cpu_cycles;

    fraction_kernel =
      (delta_cpu_counters[i].kernel * CALC_ACCURACY) / sanity_cpu_cycles;

    fraction_interrupt =
      (delta_cpu_counters[i].interrupt * CALC_ACCURACY) / sanity_cpu_cycles;


    estimated_fraction_interrupt =
      ((delta_cpu_counters[i].interrupt +
	(sanity_cpu_cycles - total_cpu_cycles)) *
       CALC_ACCURACY) / sanity_cpu_cycles;

    if (debug) {
      fprintf(where,
	      "\tfraction_idle %"PRIu64"\n"
	      "\tfraction_user %"PRIu64"\n"
	      "\tfraction_kernel %"PRIu64"\n"
	      "\tfraction_interrupt %"PRIu64"WARNING, possibly under-counted!\n"
	      "\testimated_fraction_interrupt %"PRIu64"\n",
	      fraction_idle,
	      fraction_user,
	      fraction_kernel,
	      fraction_interrupt,
	      estimated_fraction_interrupt);
    }

    /* and finally, what is our CPU utilization? */
    lib_local_per_cpu_util[i] = 100.0 - (((float)fraction_idle /
					  (float)CALC_ACCURACY) * 100.0);
#endif
    lib_local_per_cpu_util[i] *= correction_factor;
    if (debug) {
      fprintf(where,
	      "lib_local_per_cpu_util[%d] %g  cf %f\n",
	      i,
	      lib_local_per_cpu_util[i],
	      correction_factor);
    }
    lib_local_cpu_stats.cpu_util += lib_local_per_cpu_util[i];
  }
  /* we want the average across all n processors */
  lib_local_cpu_stats.cpu_util /= (float)lib_num_loc_cpus;

  if (debug) {
    fprintf(where,
	    "calc_cpu_util: returning %g\n",
	    lib_local_cpu_stats.cpu_util);
  }

  return lib_local_cpu_stats.cpu_util;

}
void
cpu_start_internal(void)
{
  get_cpu_counters(starting_cpu_counters);
}

void
cpu_stop_internal(void)
{
  get_cpu_counters(ending_cpu_counters);
}
