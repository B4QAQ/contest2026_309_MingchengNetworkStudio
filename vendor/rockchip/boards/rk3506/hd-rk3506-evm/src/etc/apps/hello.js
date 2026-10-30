// HD-RK3506-EVM built-in quick-app demo (romfs /etc/apps/hello.js).
// Draws a hello screen via the LVGL binding, counts seconds, and
// shows a button that logs a click.  Long BACK (button 2) exits.

console.log("hello.js: starting");

var scr = lv.screenActive();

var title = lv.labelCreate(scr);
lv.labelSetText(title, "Hello from QuickJS");
lv.objSetPos(title, 10, 10);
lv.objSetStyleTextColor(title, 0xffffff);

var counter = 0;
var count_lbl = lv.labelCreate(scr);
lv.labelSetText(count_lbl, "elapsed: 0s");
lv.objSetPos(count_lbl, 10, 40);
lv.objSetStyleTextColor(count_lbl, 0x66ccff);

lv.setTimeout(function tick() {
  counter++;
  lv.labelSetText(count_lbl, "elapsed: " + counter + "s");
  lv.setTimeout(tick, 1000);
}, 1000);

var btn = lv.buttonCreate(scr);
lv.objSetPos(btn, 10, 70);
lv.objSetSize(btn, 120, 40);
lv.objSetStyleBgColor(btn, 0x2266cc);

var btn_lbl = lv.labelCreate(btn);
lv.labelSetText(btn_lbl, "Click me");
lv.objCenter(btn_lbl);

lv.objAddEventCb(btn, function() {
  console.log("button clicked!");
  lv.labelSetText(btn_lbl, "Clicked!");
}, lv.EVENT_CLICKED);

// Let the J9 keys reach this button (focus navigation).
lv.groupAddObj(btn);
lv.groupFocusObj(btn);

console.log("hello.js: UI ready, long BACK to exit");
