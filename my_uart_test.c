#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <termios.h>
#include <sys/ioctl.h>


#define DEFAULT_DEV "/dev/tty_my_uart0"
#define DEFAULT_PROC "/proc/myuart"

#define DEFAULT_BAUD    115200
#define DEFAULT_COUNT   5
#define DEFAULT_LEN     16

#define MAX_BUF_SIZE    4096

static const char *dev_name = DEFAULT_DEV;
static int baudrate = DEFAULT_BAUD;
static int test_count = DEFAULT_COUNT;
static int data_len = DEFAULT_LEN;

/* 串口 fd */
static int uart_fd = -1;

/* 函数声明 */

/* 1. 打开 /dev/tty_my_uart0 */
static int uart_open(const char *dev);

/* 2. 配置串口：波特率、校验位、流控 */
static int uart_config(int fd, int baudrate);

/* 3. 循环写入指定长度测试数据 */
static int uart_write_once(int fd, int len, int index);

/* 4. 阻塞读取串口返回数据 */
static int uart_read_once(int fd, int len);

/* 5. 读取 /proc/myuart 统计信息 */
static int proc_read_stats(void);

/* 6. 写 reset 到 /proc/myuart，清零统计 */
static int proc_reset_stats(void);

/* 7. 解析命令行参数：控制次数和长度 */
static int parse_args(int argc, char *argv[]);

/* 波特率转换：115200 -> B115200 */
static speed_t baudrate_to_speed(int baudrate);

/* 生成测试数据 */
static void make_test_data(char *buf, int len, int index);

int main(int argc, char *argv[])
{
    int ret;
    int i;

    ret = parse_args(argc, argv);//解析命令行参数
    if (ret < 0)
        return -1;

    uart_fd = uart_open(DEFAULT_DEV);//打开文件
    if (uart_fd < 0)
        return -1;

    ret = uart_config(uart_fd, baudrate);//配置串口，波特率
    if (ret < 0)
        goto out_close;

    proc_read_stats();

    for (i = 0; i < test_count; i++) {
        ret = uart_write_once(uart_fd, data_len, i);
        if (ret < 0)
            break;

        ret = uart_read_once(uart_fd, data_len);
        if (ret < 0)
            break;
    }

    proc_read_stats();

    // proc_reset_stats();

    // proc_read_stats();

out_close:
    close(uart_fd);
    return ret;
}


/* 1. 打开 /dev/tty_my_uart0 */
static int uart_open(const char *dev)
{
    int fd;
    fd = open(dev, O_RDWR | O_NOCTTY);
    if(fd < 0)
    {
        perror("open uart");
        return -1;
    }
    printf("Open UART: %s success\n", dev);

    return fd;
}

/* 2. 配置串口：波特率、校验位、流控 */
static int uart_config(int fd, int baudrate)
{
    struct termios tty;
    speed_t speed;
    /* 读取当前串口配置 */
    if (tcgetattr(fd, &tty) != 0) 
    {
    perror("tcgetattr");
    return -1;
    }
    /* 波特率转换 */
    speed = baudrate_to_speed(baudrate);

    /* 设置输入输出波特率 */
    cfsetispeed(&tty, speed);

    cfsetospeed(&tty, speed);

    /* 设置数据位 */
    tty.c_cflag &= ~CSIZE;
    tty.c_cflag |= CS8;

    /* 设置停止位 */
    tty.c_cflag &= ~CSTOPB;

    /* 设置校验位 */
    tty.c_cflag &= ~PARENB;

    /* 设置流控 */
    tty.c_cflag &= ~CRTSCTS; //关闭硬件
    tty.c_iflag &= ~(IXON | IXOFF | IXANY); //关闭软件

    /*
    CRTSCTS：RTS/CTS 硬件流控
    IXON：发送 XON/XOFF
    IXOFF：接收 XON/XOFF
    IXANY：任意字符恢复发送
    */

    /* 设置本地模式 */
    tty.c_lflag = 0;
    tty.c_oflag = 0;
    /* 设置阻塞读取 */
    tty.c_cc[VMIN] = 1;
    tty.c_cc[VTIME] = 0;
    /* 刷新缓冲区 */
    tcflush(fd, TCIOFLUSH);
    /* 写回配置 */
    if (tcsetattr(fd, TCSANOW, &tty) != 0)
    {
        perror("tcsetattr");
        return -1;
    }
    return 0;

}

/* 3. 循环写入指定长度测试数据 */
static int uart_write_once(int fd, int len, int index)//index是第几次写入，len是写入长度
{
    char buf[MAX_BUF_SIZE];
    int ret;
    if(len <= 0 || len > MAX_BUF_SIZE)
    {
        printf("invalid write length: %d\n", len);
        return -1;
    }
    make_test_data(buf, len, index);
    //生成测试数据,这个函数会根据index生成不同的测试数据，比如第0次写入全是A，第1次写入全是B，第2次写入全是C，以此类推

    ret = write(fd, buf, len);//写入串口
    if (ret < 0) {
        perror("write uart");
        return -1;
    }

    printf("[TX] index=%d write=%d bytes\n", index, ret);

    return ret;
}

/* 4. 阻塞读取串口返回数据 从串口读取一次数据，并返回实际读取的字节数。*/
static int uart_read_once(int fd, int len)
{
    char buf[MAX_BUF_SIZE];
    int ret;
    if (len <= 0 || len > MAX_BUF_SIZE) {
        printf("invalid read length: %d\n", len);
        return -1;
    }

    ret = read(fd, buf, len);//阻塞读取串口数据
    if (ret < 0) {
        perror("read uart");
        return -1;
    }

    printf("[RX] read=%d bytes\n", ret);

    return ret;
}

/* 5. 读取 /proc/myuart 统计信息 */
static int proc_read_stats(void)
{
    int fd;
    char buf[1024];
    int ret;

    fd = open(DEFAULT_PROC, O_RDONLY);//打开 /proc/myuart
    if (fd < 0) {
        perror("open proc");
        return -1;
    }

    ret = read(fd, buf, sizeof(buf));//读取 /proc/myuart 统计信息
    if (ret < 0) {
        perror("read proc");
        return -1;
    }
    close(fd);
    return 0;

}

/* 6. 写 reset 到 /proc/myuart，清零统计 */
static int proc_reset_stats(void)
{
    int fd;
    int ret;

    fd = open(DEFAULT_PROC, O_WRONLY);//打开 /proc/myuart
    if (fd < 0) {
        perror("open proc");
        return -1;
    }
    ret = write(fd, "reset", 5);//写入 reset 到 /proc/myuart，清零统计
    if (ret < 0) {
        perror("write proc");
        return -1;
    }
    close(fd);
    return 0;
}

/* 7. 解析命令行参数：控制次数和长度 */
static int parse_args(int argc, char *argv[])
{
    if(argc >= 2)
    {
        test_count = atoi(argv[1]);//获取测试次数
    }
    if(argc >= 3)
    {
        data_len = atoi(argv[2]);
    }
    printf("test count: %d", test_count);//打印测试次数
    printf("Data Length: %d",data_len);//打印数据长度

    return 0;
}

/* 波特率转换：115200 -> B115200 */
static speed_t baudrate_to_speed(int baudrate)
{
    switch (baudrate) {
    case 9600:
        return B9600;
    case 19200:
        return B19200;
    case 38400:
        return B38400;
    case 57600:
        return B57600;
    case 115200:
        return B115200;
    default:
        printf("Unsupported baudrate %d, use 115200\n", baudrate);
        return B115200;
    }
}

/* 生成测试数据 */
static void make_test_data(char *buf, int len, int index)
{
    int i;

    for (i = 0; i < len; i++) {
        buf[i] = 'A' + (index % 26);
    }

}

