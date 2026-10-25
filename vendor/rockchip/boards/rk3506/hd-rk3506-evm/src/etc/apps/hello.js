// HD-RK3506-EVM built-in quick-app demo (romfs /etc/apps/hello.js).
// Prints a hello plus a few live system facts to the syslog/console.

console.log("hello from quickjs on hd-rk3506-evm");

function readfile(p)
{
  var f = std.open(p, "r");
  if (!f) return null;
  var s = f.readAsString();
  f.close();
  return s;
}

var up = readfile("/proc/uptime");
if (up) console.log("uptime: " + up.replace(/\n/g, ""));

var load = readfile("/proc/cpuload");
if (load) console.log("cpuload: " + load.replace(/\n/g, ""));

console.log("demo done");
