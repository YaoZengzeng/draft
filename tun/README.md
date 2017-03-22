1. run `make` to get the binary file `tun`

2. on server side:
	(1) run `./tun -s`
	(2) `ip addr add 10.0.0.2/24 dev tun0 peer 10.0.0.1/24` -> configure the tun device
	(3) `ip link dev tun0 up` -> make tun device up

3. On client side:
	(1) run `./tun -c`
	(2) `ip addr add 10.0.0.1/24 dev tun0 peer 10.0.0.2/24` -> configure the tun device
	(3) `ip link dev tun0 up` -> make tun device up

4. test by running `ping 10.0.0.2` in client or running `ping 10.0.0.1` in server, it will work.

ps: replace the REMOTEIP value in source code to your server's ip address.

