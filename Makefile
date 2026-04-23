all:
	gcc tracker.c -o tracker
	gcc peer.c -o peer -lpthread