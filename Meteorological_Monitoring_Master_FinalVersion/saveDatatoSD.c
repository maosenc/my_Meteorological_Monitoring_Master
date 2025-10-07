//将数据保存到SD卡中
#include <stdio.h>
#include <stdlib.h>

int main(){
    //打开文件 如果文件不存在则创建
    FILE *file = fopen("/mnt/SD/data.txt","w");

    if (file == NULL) {
        perror("Failed to open file");
        return -1;
    }

    // 写入数据
    fprintf(file, "Hello, Embedded Linux!\n");
    fprintf(file, "This is some data being saved to SD card.\n");

    // 关闭文件
    fclose(file);

    printf("Data has been saved to SD card.\n");

    return 0;

}