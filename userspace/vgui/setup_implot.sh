#!/usr/bin/env bash
# vgui/setup_implot.sh
# Downloads the ImPlot source files needed to build vgui.
# Run once from the vgui/ directory before running make.

set -euo pipefail

IMPLOT_TAG="v1.0"
RAW="https://raw.githubusercontent.com/epezent/implot/${IMPLOT_TAG}"

FILES=(
    "implot.h"
    "implot.cpp"
    "implot_internal.h"
    "implot_items.cpp"
    "LICENSE"
)

mkdir -p implot

for f in "${FILES[@]}"; do
    echo "  Downloading ${f} ..."
    curl -fsSL -o "implot/${f}" "${RAW}/${f}"
done

echo "  Applying local freestanding compatibility patch ..."
tmp_file="$(mktemp)"
awk '
BEGIN { injected = 0 }
!injected && $0 ~ /^ImPlotTime MkGmtTime\(struct tm \*ptm\) \{$/ {
    print "#ifndef _WIN32"
    print "namespace {"
    print ""
    print "auto days_from_civil(long long year, unsigned month, unsigned day) -> long long"
    print "{"
    print "    year -= month <= 2 ? 1 : 0;"
    print "    const long long era = (year >= 0 ? year : year - 399) / 400;"
    print "    const unsigned year_of_era = static_cast<unsigned>(year - era * 400);"
    print "    const int shifted_month = static_cast<int>(month) + (month > 2 ? -3 : 9);"
    print "    const unsigned day_of_year = static_cast<unsigned>((153 * shifted_month + 2) / 5) + day - 1;"
    print "    const unsigned day_of_era = year_of_era * 365 + year_of_era / 4 - year_of_era / 100 + day_of_year;"
    print "    return era * 146097 + static_cast<long long>(day_of_era) - 719468;"
    print "}"
    print ""
    print "auto portable_timegm(const tm* ptm) -> time_t"
    print "{"
    print "    if (ptm == nullptr) {"
    print "        return static_cast<time_t>(-1);"
    print "    }"
    print ""
    print "    const long long days = days_from_civil(static_cast<long long>(ptm->tm_year) + 1900,"
    print "                                           static_cast<unsigned>(ptm->tm_mon) + 1,"
    print "                                           static_cast<unsigned>(ptm->tm_mday));"
    print "    const long long seconds = (((days * 24) + ptm->tm_hour) * 60 + ptm->tm_min) * 60 + ptm->tm_sec;"
    print "    return static_cast<time_t>(seconds);"
    print "}"
    print ""
    print "} // namespace"
    print "#endif"
    print ""
    injected = 1
}
{ print }
' implot/implot.cpp > "${tmp_file}"
mv "${tmp_file}" implot/implot.cpp
sed -i 's/    t.S = timegm(ptm);/    t.S = portable_timegm(ptm);/' implot/implot.cpp

echo ""
echo "ImPlot ${IMPLOT_TAG} downloaded to ./implot/"
echo "You can now run:  make"
