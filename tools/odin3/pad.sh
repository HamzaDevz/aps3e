# Runs on the device. Injects PS3 buttons through the Odin Controller (aPS3e reads it as an Android gamepad;
# Vendor_2020_Product_0111.kl maps 0x130-0x134 to BUTTON_A/B/C/X/Y, which aPS3e binds to cross/circle/-/square/triangle).
#   sh pad.sh btn <cross|circle|triangle|square|l|r|select|start|ps|up|down|left|right> [hold_ms]
#   sh pad.sh stick|rstick <x -32767..32767> <y, <0 = up> <hold_ms>
E=$(getevent -pl 2>/dev/null | grep -B1 '"Odin Controller"' | sed -n 's/^add device [0-9]*: //p')
E=${E:-/dev/input/event8}
syn() { sendevent $E 0 0 0; }
ms() { usleep $(( $1 * 1000 )); }
case $1 in
  btn)
    case $2 in
      cross) k=304;; circle) k=305;; square) k=307;; triangle) k=308;; l) k=310;; r) k=311;;
      select) k=314;; start) k=315;; ps) k=316;;
      up) h=17; v=-1;; down) h=17; v=1;; left) h=16; v=-1;; right) h=16; v=1;;
      *) echo "unknown button $2" >&2; exit 2;;
    esac
    if [ -n "$h" ]; then
      sendevent $E 3 $h $v; syn; ms ${3:-120}; sendevent $E 3 $h 0; syn
    else
      sendevent $E 1 $k 1; syn; ms ${3:-120}; sendevent $E 1 $k 0; syn
    fi;;
  stick|rstick)
    if [ $1 = stick ]; then ax=0; ay=1; else ax=2; ay=5; fi
    sendevent $E 3 $ax $2; sendevent $E 3 $ay $3; syn; ms $4
    sendevent $E 3 $ax 0; sendevent $E 3 $ay 0; syn;;
  *) echo "usage: pad.sh btn <name> [ms] | stick|rstick <x> <y> <ms>" >&2; exit 2;;
esac
