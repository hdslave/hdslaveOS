# Measuring on the device

Everything below needs Melodee running on the MPC and a shell on it (root SSH over USB-C; see `../MPC Sample Knowledge Base/05-community-mods-and-tools.md`). `device.py ctl status` prints the status line.

| Question | How |
| --- | --- |
| Are the clocks untouched? | `cat /sys/devices/system/cpu/cpufreq/policy0/scaling_{governor,max_freq,cur_freq}`: `performance`, 1800000, 1800000, as under the stock app |
| What does a block take? | `melodee_render_load=` in the status line (tenths of a percent of a block), the same project under the original Melodee and under the fork, playing and stopped |
| Dropouts | Settings > AUDIO shows them; the status line has the audio `xruns` count, `melodee_dropout_track=` and `melodee_guard=` |
| Screen thread cost | `screen_presented` (frames drawn), `screen_pushed` (frames that changed) and `screen_draw_us` (last draw) in the status line, read twice ten seconds apart on each page, stopped and playing |
| Is the screen dimmed? | `melodee_dimmed=` in the status line |
| Per-thread CPU | `top -H -p $(pidof sample-base)` for a minute, stopped and playing: the screen, service, MIDI and pager threads should be near zero when stopped |
| Wakeups | `grep ctxt /proc/$(pidof sample-base)/task/*/status` twice, a minute apart |
| Needless saves | Leave the device untouched for a minute and check the project file's time does not move |
| Battery life | Same project, same brightness, from full until it switches off: once under the original Melodee, once under the fork. The supply voltage is in `/sys/class/power_supply/mains-power/voltage_now`; logging it once a minute gives the first discharge curve anyone has published for this device |

# How this document is made

The findings and appendices are assembled from the auditors' reports, my verdicts and the fork's status by `.local/audit/build_doc.py` (the `.local` folder is not tracked by Git). To change a verdict or a status, edit `verdicts.tsv` or `status.tsv` there and run the script again.
