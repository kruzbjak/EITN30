# Project for Internet Inside course

This is a repo for a project given in Internet Inside (EITN30) course at LTH.  
We set our goal to replace the built-in ARQ mechanism with our own, while using both of the duplex links.  
Instead of the built-in stop-and-wait ARQ, this approach allows us to not use timeouts almost completely.  
Our solution is found in the ARQ directory, in ourArq.cpp file.  

## Compiling

Currently we are using *rf24* and *libtins* libraries, so to compile the .cpp files you can do:
```bash
g++ -std=c++11 codeFileName.cpp -o executableName -lrf24 -ltins
```

## Usage

Currently there are 4 folders with three example codes, and one goal/problem solution.
They are (not InterceptingPing) meant to be run on two raspberries simultaneously, one as a base station one as a mobile station.  

For running **InterceptingPing** (here only -ltins is mandatory, lrf24 not used).
Sudo needed to be able to establish a tun virtual interface.
After running the executable, run a *ping 8.8.8.8* in a different cmd.
```bash
sudo ./executable
```
For running **ConcurrentTransmission** (here only -lrf24 is mandatory, ltins not used).  
On one raspberry, there must be the --mobile version running, on the other the --base version running.
```bash
./executable --mobile
./executable --base
```
For running **TransmittingPing** (here both libraries mandatory).
Again one raspberry must be running the --mobile version and the second the base version.
Here the computer with the base version running has to have forwarding enabled.  
-> that means that in the **/etc/sysctl.conf** file there should be line with *net.ipv4.ip_forward=1* (uncommented).
```bash
sudo ./executable --mobile
sudo ./executable --base
```

For running **ARQ** (here both libraries mandatory).
The same requirements as for *TransmittingPing* apply.

## Testing

For testing the tcpdump tool can be used,
for example running:
```bash
sudo tcpdump --interface any port not 22
# OR
sudo tcpdump --interface tun0
```
## Links

[rf24](https://nrf24.github.io/RF24/)  
[libtins](https://libtins.github.io/)  
