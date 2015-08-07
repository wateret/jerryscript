#!/bin/bash

trap "exit" INT

function pr_err() {
  echo -e "\e[91mError: $@\e[39m"
}

function exit_err() {
  pr_err $@
  exit
}

USAGE="Usage:\n    sudo bash run.sh OLD_ENGINE NEW_ENGINE REPEATS"

if [ "$(whoami)" != "root" ]
then
  pr_err "start script with sudo..."
  echo -e $USAGE
  exit
fi

if [ "$#" -ne 5 ]
then
  pr_err "Argument number mismatch..."
  echo -e $USAGE
  exit
fi

CPU=3
ENGINE_OLD="$1"
ENGINE_NEW="$2"
REPEATS="$3"
SINGLE_TEST_TIMEOUT="$4"
TIMEOUT=$(echo "${REPEATS}" "${SINGLE_TEST_TIMEOUT}" | awk '{print $1 * $2;}')
BENCH_FOLDER="$5"

if [ ! -x $ENGINE_OLD ]
then
  exit_err "\"$ENGINE_OLD\" is not an executable file..."
fi

if [ ! -x $ENGINE_NEW ]
then
  exit_err "\"$ENGINE_NEW\" is not an executable file..."
fi

if [ $REPEATS -lt 1 ]
then
  exit_err "REPEATS must be greater than 0"
fi

if [ $SINGLE_TEST_TIMEOUT -lt 1 ]
then
  exit_err "SINGLE_TEST_TIMEOUT must be greater than 0"
fi

RAND_VA_SPACE="/proc/sys/kernel/randomize_va_space"
echo 0 > $RAND_VA_SPACE || exit_err "setting $RAND_VA_SPACE"

renice -n -19 $$ || exit_err "renice failed"
#cpufreq-set -g performance || exit_err "couldn't set performance governor"
#cpufreq-set -c ${CPU} -f 500MHz  || exit_err "couldn't set frequency"
taskset -p -c ${CPU} $$ || exit_err "couldn't set affinity"

perf_n=0
mem_n=0

perf_rel_mult=1.0
mem_rel_mult=1.0

function run-compare()
{
  COMMAND=$1
  PRE=$2
  TEST=$3

  OLD=$(timeout "${TIMEOUT}" ${COMMAND} "${ENGINE_OLD}" "${TEST}") || return 1
  NEW=$(timeout "${TIMEOUT}" ${COMMAND} "${ENGINE_NEW}" "${TEST}") || return 1

  #check result
  ! $OLD || ! $NEW || return 1

  #calc relative speedup
  rel=$(echo "${OLD}" "${NEW}" | awk '{print $2 / $1; }')

  #increment n
  ((${PRE}_n++))

  #accumulate relative speedup
  eval "rel_mult=\$${PRE}_rel_mult"
  rel_mult=$(echo "$rel_mult" "$rel" | awk '{print $1 * $2;}')
  eval "${PRE}_rel_mult=\$rel_mult"

  #calc percent to display
  percent=$(echo "$rel" | awk '{print (1.0 - $1) * 100; }')
  printf "%28s" "$(printf "%6s->%6s (%3.3f)" "$OLD" "$NEW" "$percent")"
}

function run-test()
{
  TEST=$1

  printf "%40s | " "${TEST##*/}"
  run-compare "./tools/rss-measure.sh"      "mem"   "${TEST}" || return 1
  printf " | "
  run-compare "./tools/perf.sh ${REPEATS}"  "perf"  "${TEST}" || return 1
  printf " | "
  printf "\n"
}

function run-suite()
{
  FOLDER=$1

  for BENCHMARK in ${FOLDER}/*.js
  do
    run-test "${BENCHMARK}" 2> /dev/null || printf "<FAILED>\n" "${BENCHMARK}";
  done
}

printf "%40s | %28s | %28s |\n" "Benchmark" "Rss" "Perf"
printf "%40s | %28s | %28s |\n" "---------" "---" "----"

run-suite "${BENCH_FOLDER}"

mem_rel_gmean=$(echo "$mem_rel_mult" "$mem_n" | awk '{print $1 ^ (1.0 / $2);}')
mem_percent_gmean=$(echo "$mem_rel_gmean" | awk '{print (1.0 - $1) * 100;}')

perf_rel_gmean=$(echo "$perf_rel_mult" "$perf_n" | awk '{print $1 ^ (1.0 / $2);}')
perf_percent_gmean=$(echo "$perf_rel_gmean" | awk '{print (1.0 - $1) * 100;}')

printf "%40s | %28s | %28s |\n" "Geometric mean:" "RSS reduction: $mem_percent_gmean%" "Speed up: $perf_percent_gmean%"
