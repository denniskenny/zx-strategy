"""P0 acceptance: walk the whole campaign, verify every level loads.

Drives the built .tap through title -> play -> ST_OVER -> next level ->
... -> ST_WON -> title using the DEBUG_STATE_WALK keys (W wins a level,
L loses it), and checks terrain[] against the .tmx at levels 1, 5 and 10.

    make map                      # symbols are read from zxstrategy.map
    zesarux --vo null --ao null --enable-remoteprotocol --machine 48k \
        --noconfigfile --quickexit --romfile ~/projects/zesarux/src/48.rom \
        $PWD/zxstrategy.tap &
    sleep 8 && python3 tests/p0_state_walk.py

Retire this with DEBUG_STATE_WALK in P4.  See docs/PLAN.md.

Note: press_until() polls for the expected state rather than sleeping a
fixed time.  Headless ZEsarUX runs well under 50 fps, so a fixed-delay
press drops roughly every other keystroke.
"""
import socket, time, re, subprocess, sys

def sym(name):
    out = subprocess.run(['grep', '-E', f'^_{name}\\b', 'zxstrategy.map'],
                         capture_output=True, text=True).stdout.split()
    return int(out[2].lstrip('$'), 16)          # addresses from the fresh map

GS, TER, LVL, WON = (sym('game_state'), sym('terrain'), sym('level'), sym('player_won'))
ST = {0:"TITLE",1:"PLAY",2:"MAP",3:"GALLERY",4:"MUSIC",5:"OVER",6:"WON"}
print(f"symbols: game_state={GS:#06x} terrain={TER:#06x} level={LVL:#06x} won={WON:#06x}")

def connect():
    s=socket.socket(); s.settimeout(25); s.connect(('localhost',10000))
    b=b''
    while b'command>' not in b: b+=s.recv(4096)
    return s
def cmd(s,c):
    s.sendall((c+'\n').encode()); r=b''
    while b'command>' not in r: r+=s.recv(4096)
    t=r.decode('latin-1'); return t[:t.rfind('command>')].strip()
def rd(s,a,n):
    x=cmd(s,f'read-memory {a} {n}').replace(' ','').replace('\n','').replace('\r','')
    return bytes.fromhex(x[:n*2])

KEY = {'ENTER':(6,0), 'W':(2,1), 'L':(6,1), 'SPACE':(7,0)}
def press(s,k,hold=0.3):
    i,b = KEY[k]; rows=[0xFF]*8; rows[i] &= ~(1<<b)&0xFF
    cmd(s,'set-ui-io-ports '+''.join(f'{x:02x}' for x in rows)+'00'); time.sleep(hold)
    cmd(s,'set-ui-io-ports ffffffffffffffff00'); time.sleep(hold)

def press_until(s, k, want, tries=6):
    """Press until the app reaches `want`.  An edge fires once per
       press-release cycle, so a repeat is safe when nothing happened."""
    for _ in range(tries):
        if want(): return True
        press(s, k)
    return want()
st = lambda s: rd(s,GS,1)[0]
lv = lambda s: rd(s,LVL,1)[0]

def expected(n):
    csv = re.search(r'<data encoding="csv">\s*(.*?)\s*</data>',
                    open(f'assets/maps/level_{n}.tmx').read(), re.S).group(1)
    return [int(v)-1 for v in csv.replace('\n','').split(',') if v.strip()]

s = connect()
cmd(s,'smartload /Users/Kennyd/projects/zx-strategy/zxstrategy.tap'); time.sleep(10)
fails = []
def check(cond, msg):
    print(("  ok   " if cond else "  FAIL ") + msg)
    if not cond: fails.append(msg)

check(st(s)==0, f"boots to TITLE (got {ST.get(st(s))})")
press_until(s,'ENTER', lambda: st(s)==1)
for lvl in range(1,11):
    check(st(s)==1 and lv(s)==lvl,
          f"level {lvl}: PLAY, level counter = {lv(s)}")
    if lvl in (1,5,10):
        check(list(rd(s,TER,98))==expected(lvl),
              f"level {lvl}: terrain[] matches assets/maps/level_{lvl}.tmx")
    press_until(s,'W', lambda: st(s)==5)
    check(st(s)==5 and rd(s,WON,1)[0]==1, f"level {lvl}: W -> ST_OVER, player_won=1")
    want = 6 if lvl==10 else 1
    press_until(s,'ENTER', lambda: st(s)==want)
check(st(s)==6, f"winning level 10 -> ST_WON (got {ST.get(st(s))})")
press_until(s,'SPACE', lambda: st(s)==0)
check(st(s)==0, "any key from ST_WON -> ST_TITLE")

press_until(s,'ENTER', lambda: st(s)==1)
press_until(s,'W', lambda: st(s)==5)
press_until(s,'ENTER', lambda: st(s)==1)
check(st(s)==1 and lv(s)==2, f"restart, win once -> level {lv(s)}")
press_until(s,'L', lambda: st(s)==5)
check(st(s)==5 and rd(s,WON,1)[0]==0, "L -> ST_OVER with player_won=0")
press_until(s,'ENTER', lambda: st(s)==0)
check(st(s)==0, "a loss returns to ST_TITLE")
press_until(s,'ENTER', lambda: st(s)==1)
check(st(s)==1 and lv(s)==1, f"a new game restarts at level {lv(s)}")

print("\nP0 ACCEPTANCE:", "PASS" if not fails else f"FAIL ({len(fails)})")
sys.exit(1 if fails else 0)
