#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <linux/cdrom.h>

void print_layer(struct dvd_layer layer, unsigned i)
{
	(void)i;
	printf("book_version: %d\n", layer.book_version);
	printf("book_type: %d\n", layer.book_type);
	printf("min_rate: %d\n", layer.min_rate);
	printf("disc_size: %d\n", layer.disc_size);
	printf("layer_type: %d\n", layer.layer_type);
	printf("track_path: %d\n", layer.track_path);
	printf("nlayers: %d\n", layer.nlayers);
	printf("track_density: %d\n", layer.track_density);
	printf("bca: %d\n", layer.bca);
	printf("start_sector: 0x%X\n", layer.start_sector);
	printf("end_sector: 0x%X\n", layer.end_sector);
	printf("end_sector_l0: 0x%X\n", layer.end_sector_l0);
}

int main(int argc, char *argv[])
{
	char *dvd_filename = "/dev/dvd";
	if (argc == 1) {
	} else if (argc == 2) {
		dvd_filename = argv[1];
	}

	int fd = open(dvd_filename, O_RDONLY);
	if (fd < 0) {
		fprintf(stderr, "Couldn't open %s: %s\n", dvd_filename, strerror(errno));
		return 1;
	}

	for (unsigned i = 0; i < 4; i++) {
		dvd_struct dvd = {0};
		dvd.type = DVD_STRUCT_PHYSICAL;
		dvd.physical.layer_num = (__u8)i;
		int e = ioctl(fd, DVD_READ_STRUCT, &dvd);
		if (e < 0) {
			fprintf(stderr, "Couldn't get layer %d info: %s\n", i, strerror(errno));
			continue;
		}

		print_layer(dvd.physical.layer[i], i);
		putc('\n', stdout);
	}

	return 0;
}
