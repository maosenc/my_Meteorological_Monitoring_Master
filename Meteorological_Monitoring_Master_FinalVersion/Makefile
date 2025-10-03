# 编译器和编译选项
CC = gcc
CFLAGS = -Wall -g

# 源文件和目标文件
recv_SRC = receiver_with_shm.c
recv_OBJ = receiver_with_shm
send_SRC = sender.c
send_OBJ = sender 
serv_SRC = server.c
serv_OBJ = server

# 目标文件夹
OUT_DIR = ./output

# 默认目标
all: $(OUT_DIR)/$(recv_OBJ) $(OUT_DIR)/$(send_OBJ) $(OUT_DIR)/$(serv_OBJ)
recv:$(OUT_DIR)/$(recv_OBJ)
send:$(OUT_DIR)/$(send_OBJ)
serv:$(OUT_DIR)/$(serv_OBJ)


# 创建输出目录
$(OUT_DIR):
	mkdir -p $(OUT_DIR)

# 编译目标
$(OUT_DIR)/$(recv_OBJ): $(recv_SRC) | $(OUT_DIR)
	arm-linux-gnueabihf-$(CC) $(CFLAGS) -std=c99 -D_XOPEN_SOURCE $(recv_SRC) -o $@

$(OUT_DIR)/$(send_OBJ): $(send_SRC) | $(OUT_DIR)
	$(CC) $(CFLAGS) $(send_SRC) -o $@ -lm

$(OUT_DIR)/$(serv_OBJ): $(serv_SRC) | $(OUT_DIR)
	$(CC) $(CFLAGS) $(serv_SRC) -o $@

# 清理目标
clean:
	rm -rf $(OUT_DIR)

# 伪目标
.PHONY: all clean recv send serv
