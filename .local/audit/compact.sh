#!/bin/sh
# Print each finding's title and its short fields from an audit report.
awk '
/^### /{print ""; print $0; next}
/^- \*\*(Where|Thread and frequency|Gain|Sound or behaviour change|Risk|Effort|Confidence):\*\*/{ s=$0; if (length(s)>330) s=substr(s,1,330) "…"; print s }
' "$1"
