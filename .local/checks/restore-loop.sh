SAVED=/run/sample-base-backlight
if [ -r "$SAVED" ]; then
    while read -r DEVICE VALUE; do
        case "$DEVICE" in *..*) continue ;; esac
        case "$DEVICE" in /sys/class/backlight/?*) ;; *) continue ;; esac
        case "$VALUE" in ''|*[!0-9]*) continue ;; esac
        { [ ! -w "$DEVICE/brightness" ] || echo "$VALUE" > "$DEVICE/brightness"; } 2>/dev/null || true
    done < "$SAVED"
    rm -f "$SAVED"
fi
