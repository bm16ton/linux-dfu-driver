#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>
#include <string.h>

const char *default_devname = "/dev/dfu0";

int main(int argc, char *argv[])
{
	const char *devname;
	int dfuh, numb;
	char resp[8];
	struct cmdinfo {
		unsigned char cmd[8];
	} cmd;
	struct devinfo {
		unsigned short usFlashBlockSize;
		unsigned short usNumFlashBlocks;
		unsigned int ulPartInfo;
		unsigned int ulClassInfo;
		unsigned int ulFlashTop;
		unsigned int ulAppStartAddr;
	} dinfo;

	if (argc > 1)
		devname = argv[1];
	else
		devname = default_devname;

	dfuh = open(devname, O_RDWR);
	if (dfuh < 0) {
		fprintf(stderr, "Cannot open device: %s->%d\n", devname,
			errno);
	}

	memset(cmd.cmd, 0, sizeof(cmd));
	cmd.cmd[0] = 0x05;
	numb = write(dfuh, cmd.cmd, sizeof(cmd));
	printf("Command written: %d. Look at the state\n", numb);
	scanf("%s", resp);
	numb = read(dfuh, &dinfo, sizeof(dinfo));
	printf("Result read: %d. Look at the state\n", numb);
	scanf("%s", resp);

	printf("block size: %d, number of blocks: %d, Part Info: %8.8X, Class Info: %8.8X, Top: %8.8X, Start: %8.8X\n",
		(int)dinfo.usFlashBlockSize, (int)dinfo.usNumFlashBlocks,
		dinfo.ulPartInfo, dinfo.ulClassInfo,
		dinfo.ulFlashTop, dinfo.ulAppStartAddr);

	close(dfuh);
	return 0;
}
