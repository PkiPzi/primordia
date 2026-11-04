#!/usr/bin/env bash
# Seed sweep: speed (million creature-ticks/s, comparable between builds) and novelty (species, discoveries).
# usage: tools/bench.sh [binary] [ticks] [seeds...]     e.g. tools/bench.sh build/primordia 20000 1 2 3
BIN=${1:-build/primordia}; TICKS=${2:-20000}; shift 2 2>/dev/null; SEEDS=${*:-1 2 3}
printf "%-6s %9s %8s %6s %8s %8s %6s\n" seed ticks/s Mcr-t/s pop species discov secs
tot=0; n=0
for s in $SEEDS; do
  line=$("$BIN" --headless --ticks "$TICKS" --seed "$s" 2>&1 | grep '^RESULT')
  eval "$(echo "$line" | tr ' ' '\n' | grep = | sed 's/^/r_/')"
  printf "%-6s %9s %8s %6s %8s %8s %6s\n" "$r_seed" "$r_ticks_per_s" "$r_mcreature_ticks_per_s" "$r_pop" "$r_species_alive" "$r_discoveries" "$r_secs"
  tot=$(awk -v a="$tot" -v b="$r_mcreature_ticks_per_s" 'BEGIN{print a+b}'); n=$((n+1))
done
awk -v t="$tot" -v n="$n" 'BEGIN{printf "mean Mcreature-ticks/s: %.3f\n", t/n}'
