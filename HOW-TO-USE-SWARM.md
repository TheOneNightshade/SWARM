# SWARM: How to use it

SWARM turns a few small boards ("nodes") and your Android phone into one wardriving rig. Each node
scans Wi-Fi and Bluetooth and sends what it finds to your phone. The phone adds the GPS position,
scans too, and saves everything in one log you can upload to WiGLE, WDGWars or WarDrift.

This guide is for app build 29.

---

## 1. Before your first drive (one time only)

1. **Flash your nodes.** On a computer, open the web flasher in Chrome or Edge:
   **theonenightshade.github.io/SWARM-Node-Flasher**. Plug in a board, click **Connect & install**,
   pick the board's port, then **Install**. Do this for each board.
2. **Install the SWARM app** on your phone and open it.
3. **Allow everything it asks for:** Location, Nearby devices and Notifications.
4. **Pick "Allow all the time" for location** when SWARM asks. Then allow it to run **unrestricted**
   (battery). Without these two, GPS can stop when your phone locks.
5. **Keep your phone's Location switched on.** Android hides Wi-Fi and Bluetooth results when it's off.

---

## 2. Connect your nodes

1. Power on your nodes (USB power bank, car USB port or USB hub).
2. Open SWARM and tap **FIND MY NODES** on the **BASIC** tab.
3. Each node shows up and gets a number: the first one heard is node 1, the next is node 2, and so on.
   A node keeps its number.
4. The first time, each node **locks to your phone**. Nobody else's SWARM app can use it after that.

---

## 3. Go for a drive (the easy way)

1. Power on the nodes and open SWARM. It finds them by itself.
2. On the **BASIC** tab, tap the big **START** button.
3. The button turns **amber** while it waits for GPS, then **red** once it's scanning.
   Outside with a clear sky this usually takes under a minute.
4. Drive. You can lock the phone, turn the screen off or use other apps. SWARM keeps scanning, and
   its notification says **SWARM · SCANNING**.
5. Tap **STOP** when you're done. Your log is saved.

BASIC scans 2.4 GHz Wi-Fi, 5 GHz Wi-Fi and Bluetooth, shared out over your nodes automatically.
There's nothing to set up.

---

## 4. The tabs

| Tab | What it's for |
|---|---|
| **BASIC** | One big START / STOP button, how many nodes are connected, GPS, and your counts. Start here. |
| **ADVANCED** | Pick exactly what to log (Wi-Fi, Bluetooth, cell towers), set which node scans what, live feed and stats. |
| **MAP** | Your route and what you found along it. |
| **LOGS** | Every log on the phone: **UPLOAD**, **SAVE**, **SHARE**, **INFO**, **DELETE**. |
| **SWARM** | Your nodes and their status, the GPS source switch, node firmware updates, and DIY nodes. |
| **NEARBY** | Trackers that seem to follow you, other SWARM rigs nearby, and other wardriving devices. |

**SETTINGS** (the gear at the top right) holds spoken alerts, auto start, auto upload, units,
running in the background, your node lock key and the settings backup.

---

## 5. What the node list means

| You see | It means |
|---|---|
| **ONLINE** / **SCANNING** | The node is working. |
| **OFFLINE** | Not heard lately: out of range or powered off. |
| **SYNCING PLAN** | The node is getting its instructions. It only takes a moment. |
| **5 GHz CAPABLE** | The board can scan 5 GHz Wi-Fi. |
| **NEEDS 5 GHz BOARD** | Its plan includes 5 GHz but the board can't do it, so it skips those channels. |
| **UPLOADING N QUEUED** | It's sending finds it stored while it was away from the phone. |

If a node loses the phone during a scan (you walk away from the car, for example), it keeps
scanning and stores what it finds. It sends everything when the phone is back. Nothing is lost.

---

## 6. Phone GPS or node GPS

Some boards have their own GPS. At the bottom of **SWARM → NODES** there's a **GPS SOURCE** switch:

- **PHONE GPS** (normal): the phone's GPS places everything.
- **NODE GPS**: a node's own GPS places everything instead. The GPS readout just says **NODE**.
  START waits until that node has a fix.

---

## 7. Uploading your logs

1. Open the **LOGS** tab and tap **UPLOAD** on a log.
2. The first time, tap **ACCOUNTS** and add your WiGLE, WDGWars or WarDrift details.
3. Pick where to send it. Done.

You can also turn on **auto upload** in SETTINGS, so logs go up by themselves on Wi-Fi.

---

## 8. Updating your nodes (no cable needed)

1. Go to **SWARM → UPDATE**.
2. Pick your board type and tap **GET LATEST**.
3. Tick the nodes to update and tap **UPDATE**. They update one at a time.

SWARM also tells you when a newer node firmware is out.

---

## 9. DIY nodes

A Linux computer or Raspberry Pi running **Kismet** can join your SWARM as one more node: a DIY node.
Install the `kismet-node` program on it (see the full README, Part 10), start a scan in the app, and
its finds go into your log. You'll find them on **SWARM → DIY NODES**.

---

## 10. Using a second phone

On the first phone: **SETTINGS → NODE LOCK → COPY KEY**, and send it to yourself.
On the second phone: **SETTINGS → USE A KEY FROM ANOTHER PHONE**, and paste it.
Both phones can now run the same nodes.

**Keep that key somewhere safe.** If you reinstall the app without it, you'll have to free every node.

---

## 11. Closing SWARM

- **Back** button on the main screen, or **EXIT** on the notification: SWARM stops everything. Your log is kept.
- **Home** button: SWARM keeps scanning in the background.
- **Swiping it away** during a scan keeps it scanning. You can change that in **SETTINGS → RUN IN THE BACKGROUND**.

---

## 12. If something goes wrong

| Problem | Try this |
|---|---|
| A node never shows up | Check it has power. On **SWARM**, tap **DISCONNECT**, then **CONNECT TO NODES**. |
| Stuck waiting for GPS | Go outside or near a window. Check Location is on. On NODE GPS, the node needs its own fix. |
| GPS stops when the phone locks | **SETTINGS → RUN IN THE BACKGROUND**: set Location to **Allow all the time** and Battery to **Unrestricted**. |
| A node says it belongs to someone else | Hold the node's **BOOT** button for 5 seconds while it's on. The next phone to connect becomes its owner. |
| Swapping a dead board for a new one | Tap **FORGET** on the old offline node to free its number. |
| An upload says the key was refused | Check your details under **ACCOUNTS**. |
| A node update keeps failing | Keep the phone close to the node and try again. The node keeps its old firmware until the new one is fully in. |
