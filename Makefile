
SRCS = \
	algo.c \
	ev.c \
	driver.c \
	driver_iou.c \
	main.c \
	mesh.c \
	sample.c \
	tcp.c \
	util.c

OBJ = $(patsubst %.c,%.o,$(SRCS))

IOU_DIR = /home/bsd/local/pull/liburing/src
CC = clang
INC = -I$(IOU_DIR)/include
CFLAGS = -Wall -g -O $(INC)
LIBS = $(IOU_DIR)/liburing.a -lpthread

TARGET = allr

$(TARGET): $(OBJ)
	$(CC) -o $(TARGET) $^ $(LIBS)

clean:
	rm -f $(TARGET) $(OBJ)
