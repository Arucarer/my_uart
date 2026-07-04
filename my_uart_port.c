/******************************************************************************
 * @file    my_uart_port.c
 * @brief   基于 Linux UART/TTY 标准框架的虚拟 UART 端口实现
 *
 * @author  李宇坤
 * @date    2026-07
 * @version V1.0
 *
 * @details
 * 本文件负责自定义 UART 端口（uart_port）的具体功能实现，
 * 基于 Linux UART 子系统（serial_core）提供的 struct uart_ops
 * 标准接口完成虚拟 UART 数据通路。
 *
 * 主要功能包括：
 * 1. 实现 struct uart_ops 全部标准回调接口；
 * 2. 实现虚拟 UART TX/RX FIFO 数据收发逻辑；
 * 3. 实现发送、接收、启动、关闭等端口控制功能；
 * 4. 支持 termios 串口参数配置（波特率、数据位、校验位等）；
 * 5. 实现虚拟串口回环、阻塞读写及等待队列唤醒机制；
 * 6. 在 UART 收发过程中维护全局读写字节统计信息。
 *
 * 本文件主要负责 UART 端口的数据收发及控制逻辑，
 * 不负责 UART 驱动注册、模块加载卸载及 proc 调试接口，
 * 这些功能分别由 my_uart_core.c 和 my_uart_proc.c 实现。
 ******************************************************************************/

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/serial_core.h>
#include <linux/serial.h>
#include <linux/tty.h>
#include <linux/tty_flip.h>
#include <linux/spinlock.h>
#include "my_uart.h"

/* uart_ops 回调函数声明 */

static unsigned int my_uart_tx_empty(struct uart_port *port);
static void my_uart_set_mctrl(struct uart_port *port, unsigned int mctrl);
static unsigned int my_uart_get_mctrl(struct uart_port *port);
static void my_uart_stop_tx(struct uart_port *port);
static void my_uart_start_tx(struct uart_port *port);
static void my_uart_stop_rx(struct uart_port *port);
// static void my_uart_start_rx(struct uart_port *port);
static void my_uart_break_ctl(struct uart_port *port, int break_state);
static int my_uart_startup(struct uart_port *port);
static void my_uart_shutdown(struct uart_port *port);
static void my_uart_set_termios(struct uart_port *port, struct ktermios *termios, struct ktermios *old);
static void my_uart_config_port(struct uart_port *port, int flags);
static int my_uart_verify_port(struct uart_port *port, struct serial_struct *ser);



/*先写 uart_ops 空框架*/
const struct uart_ops my_uart_ops = {
    .tx_empty   = my_uart_tx_empty,//判断发送FIFO是否为空
    .set_mctrl  = my_uart_set_mctrl,//设置调制解调器控制信号（CTS/RTS/DTR等）
    .get_mctrl  = my_uart_get_mctrl,//读取调制解调器状态
    .stop_tx    = my_uart_stop_tx,//启停发送通路
    .start_tx   = my_uart_start_tx,//启停发送通路
    .stop_rx    = my_uart_stop_rx,//启停接收通路
    // .start_rx   = my_uart_start_rx,//启停接收通路
    .break_ctl  = my_uart_break_ctl,//发送串口BREAK信号
    .startup    = my_uart_startup,//串口打开时初始化硬件/虚拟缓存
    .shutdown   = my_uart_shutdown,//串口关闭时释放资源
    .set_termios = my_uart_set_termios, //配置串口波特率、数据位、校验、停止位、流控（termio标准参数）
    .config_port = my_uart_config_port, //端口基础初始化
    .verify_port = my_uart_verify_port, //校验termios配置合法性
};


static unsigned int my_uart_tx_empty(struct uart_port *port)//判断一下这个FIFO里面还有没有数据
{
    struct my_uart_port *my_port;
    unsigned long flags;//用来存放cpu信息
    unsigned int empty;

    my_port = container_of(port, struct my_uart_port, port);

    spin_lock_irqsave(&my_port->lock, flags);//上锁

    empty = (my_port->tx_head == my_port->tx_tail);//头指针和尾指针指向一个为空

    spin_unlock_irqrestore(&my_port->lock, flags);

    if(empty)
    {
        return TIOCSER_TEMT;
    }
    return 0;
}

static void my_uart_set_mctrl(struct uart_port *port, unsigned int mctrl)//设置调制解调器控制信号（CTS/RTS/DTR等）Linux 告诉驱动，请设置这些控制信号。
{
    struct my_uart_port *my_port;
    unsigned long flags;
    my_port = container_of(port, struct my_uart_port, port);

    spin_lock_irqsave(&my_port->lock, flags);//上锁

    my_port->mctrl = mctrl;//这只是一个虚拟的UART，所以这个mctrl只是一个保存当前状态的内容
    /*
    如果在真实的UART驱动里面，真实的连接会有相互握手
    RTS（Request To Send）请求发送、CTS（Clear To Send）允许发送、DTR（Data Terminal Ready）终端准备好了、DSR（Data Set Ready）设备准备好了、DCD（Data Carrier Detect）检测到载波
    */
   spin_unlock_irqrestore(&my_port->lock, flags);
}

static unsigned int my_uart_get_mctrl(struct uart_port *port)//Linux 问驱动，这些控制信号现在是什么状态，读取调制解调器状态
{
    struct my_uart_port *my_port;
    unsigned long flags;
    unsigned int mctrl;
    my_port = container_of(port, struct my_uart_port, port);

    spin_lock_irqsave(&my_port->lock, flags);//上锁
    mctrl = my_port->mctrl;
    spin_unlock_irqrestore(&my_port->lock, flags);
    return mctrl;
}

static void my_uart_stop_tx(struct uart_port *port)//停发送通路
{
    /* 正常的真实 UART：
    * stop_tx() 用于停止 UART 发送。
    * 一般会关闭 TX 中断、禁止发送器或关闭 DMA 发送，
    * 通过写 UART 控制寄存器实现停止发送。
    *
    * 本驱动为虚拟 UART，没有真实硬件寄存器和 TX 中断，
    * 因此此处无需任何操作。
    */
    struct my_uart_port *my_port;
    unsigned long flags;
    my_port = container_of(port, struct my_uart_port, port);
    spin_lock_irqsave(&my_port->lock, flags);
    my_port->tx_enabled = false; // 禁止发送
    spin_unlock_irqrestore(&my_port->lock, flags);

    pr_debug("my_uart: stop tx\n");
}

static void my_uart_start_tx(struct uart_port *port)
{
    /* 真实 UART：
     * 开启 TX 中断或发送器，
     * 将数据写入硬件 FIFO，由 UART 开始发送。
     *
     * 虚拟 UART：
     * 当前无真实硬件，无需开启发送。
     * 后续将在此函数中实现软件 FIFO 数据发送及回环逻辑。
     */   
    struct my_uart_port *my_port;
    struct circ_buf *xmit;//它是 Linux serial_core 的发送缓冲区
    unsigned long flags;
    unsigned char ch;//要发送的字符

    my_port = container_of(port, struct my_uart_port, port);

    if(port->state == NULL)//端口没有打开
        return;
    
    xmit = &port->state->xmit;//获取发送缓冲区
    spin_lock_irqsave(&my_port->lock, flags);
    my_port->tx_enabled = true;

    while(!uart_circ_empty(xmit))
    {
        unsigned int tx_next;
        unsigned int rx_next;
        tx_next = (my_port->tx_head + 1) & (MY_UART_FIFO_SIZE - 1);//tx_netx 是虚拟 FIFO 的下一个位置
        if (tx_next == my_port->tx_tail)//xmit 缓冲区已满
        {
            break;
        }
        ch = xmit->buf[xmit->tail];//获取xmit缓冲区的尾指针指向的字符
        xmit->tail = (xmit->tail + 1) & (UART_XMIT_SIZE - 1);//尾指针移动1位

        /* xmit -> tx_buf */
        my_port->tx_buf[my_port->tx_head] = ch;//把要发送的字符放入虚拟 FIFO 当前tx_head指向的位置
        my_port->tx_head = tx_next;//更新虚拟 FIFO 的头指针

        port->icount.tx++;//发送计数加1
        atomic64_inc(&my_port->total_tx_bytes);//累积发送字节计数

        if(my_port->rx_enabled)//接收通路开启，允许回环到 RX
        {
            rx_next = (my_port->rx_head + 1) & (MY_UART_FIFO_SIZE - 1);//rx_next 是虚拟 FIFO 的下一个位置
            if(rx_next == my_port->rx_tail)//虚拟 FIFO 已满
                break;

            /* tx_buf -> rx_buf */
            ch = my_port->tx_buf[my_port->tx_tail];//获取要发送的字符，这是从 tx_buf 取出要发送的字符
            my_port->tx_tail = (my_port->tx_tail + 1) & (MY_UART_FIFO_SIZE - 1);//更新虚拟 FIFO 的尾指针

            my_port->rx_buf[my_port->rx_head] = ch;//把要发送的字符放入虚拟 FIFO
            my_port->rx_head = rx_next;//更新虚拟 FIFO 的头指针
            
            /* rx_buf -> tty_buf */
            ch = my_port->rx_buf[my_port->rx_tail];//获取要发送的字符
            my_port->rx_tail = (my_port->rx_tail + 1) & (MY_UART_FIFO_SIZE - 1);//更新虚拟 FIFO 的尾指针

            port->icount.rx++;//接收计数加1
            atomic64_inc(&my_port->total_rx_bytes);//累积接收字节计数

            tty_insert_flip_char(&port->state->port, ch, TTY_NORMAL);//把要发送的字符放入 TTY 的翻转缓冲区
        }
    }

    spin_unlock_irqrestore(&my_port->lock, flags);

    tty_flip_buffer_push(&port->state->port);//通知 TTY 层有数据可以读取

    if (uart_circ_chars_pending(xmit) < WAKEUP_CHARS)//通知上层："发送缓冲区快空了，可以继续写数据了。"WAKEUP_CHARS是一个阈值，表示发送缓冲区中剩余的字符数，当剩余字符数小于这个阈值时，就会通知上层可以继续写数据了。
        uart_write_wakeup(port);

    pr_debug("my_uart: start tx\n");
}

static void my_uart_stop_rx(struct uart_port *port)//停止 UART 接收数据。
{
    /* 真实 UART：
     * 关闭 RX 中断或禁止接收器。
     *
     * 虚拟 UART：
     * 当前无真实接收硬件，无需处理。
     */
    struct my_uart_port *my_port;
    unsigned long flags;
    my_port = container_of(port, struct my_uart_port, port);
    spin_lock_irqsave(&my_port->lock, flags);
    my_port->rx_enabled = false; // 禁止发送
    spin_unlock_irqrestore(&my_port->lock, flags);

    pr_debug("my_uart: stop rx\n");    
}

// static void my_uart_start_rx(struct uart_port *port)//运行uart接受数据
// {
//     /* 真实 UART：
//      * 开启 RX 中断或接收器，
//      * 允许 UART 接收外部数据。
//      *
//      * 虚拟 UART：
//      * 当前无真实接收硬件，
//      * 后续由软件回环模拟接收过程。
//      */
//     struct my_uart_port *my_port;
//     unsigned long flags;
//     my_port = container_of(port, struct my_uart_port, port);
//     spin_lock_irqsave(&my_port->lock, flags);
//     my_port->rx_enabled = true; // 禁止发送
//     spin_unlock_irqrestore(&my_port->lock, flags);

//     pr_debug("my_uart: start rx\n");  
// }

static void my_uart_break_ctl(struct uart_port *port, int break_state)
{
    /* 真实 UART：
    * 控制 UART 发送 BREAK 信号。
    * BREAK 表示 TX 引脚持续输出低电平，
    * 通常通过设置 UART 控制寄存器实现。
    *
    * 虚拟 UART：
    * 无真实 TX 引脚，无需处理。
    */
    struct my_uart_port *my_port;
    unsigned long flags;

    my_port = container_of(port, struct my_uart_port, port);

    spin_lock_irqsave(&my_port->lock, flags);

    my_port->break_enable = (break_state != 0);

    spin_unlock_irqrestore(&my_port->lock, flags);

    pr_debug("my_uart: break ctl %s\n", break_state ? "on" : "off");
}

static int my_uart_startup(struct uart_port *port)//用户打开串口时调用
{
    /* 真实 UART：
    * 串口第一次被打开时调用。
    * 一般完成 UART 硬件初始化，包括：
    * 1. 申请中断（request_irq）
    * 2. 开启时钟（clk_prepare_enable）
    * 3. 初始化硬件 FIFO
    * 4. 配置 UART 控制寄存器
    * 5. 开启 UART 接收功能及 RX 中断
    *
    * 虚拟 UART：
    * 无真实硬件，仅初始化软件资源。
    * 清空 TX/RX FIFO，保证每次打开串口时缓存处于初始状态。
    */
    struct my_uart_port *my_port;
    unsigned long flags;
    my_port = container_of(port, struct my_uart_port, port);

    spin_lock_irqsave(&my_port->lock, flags);//上锁
    /* 清空虚拟 TX/RX FIFO，避免重新打开串口时残留旧数据 */

    my_port->tx_head = 0;
    my_port->tx_tail = 0;
    my_port->rx_head = 0;
    my_port->rx_tail = 0;

    /* 初始化虚拟 UART 状态 */
    my_port->tx_enabled = true;
    my_port->rx_enabled = true;
    my_port->break_enable = false;


    spin_unlock_irqrestore(&my_port->lock, flags);
    
    pr_info("my_uart: startup\n");//打印信息，告诉用户串口已经启动
    return 0;
}

static void my_uart_shutdown(struct uart_port *port)//用户关闭串口时调用
{
    /* 真实 UART：
    * 串口关闭时调用。
    * 一般释放 UART 硬件资源，包括：
    * 1. 关闭 RX/TX 中断
    * 2. 关闭 UART 控制器
    * 3. 关闭时钟（clk_disable_unprepare）
    * 4. 释放中断（free_irq）
    * 5. 清空硬件 FIFO
    *
    * 虚拟 UART：
    * 无真实硬件，仅清空软件 FIFO，
    * 恢复虚拟串口缓存到初始状态。
    */
    struct my_uart_port *my_port;
    unsigned long flags;
    my_port = container_of(port, struct my_uart_port, port);

    spin_lock_irqsave(&my_port->lock, flags);//上锁
    /* 清空虚拟 TX/RX FIFO，避免重新打开串口时残留旧数据 */

    my_port->tx_head = 0;
    my_port->tx_tail = 0;
    my_port->rx_head = 0;
    my_port->rx_tail = 0;

    /* 初始化虚拟 UART 状态 */
    my_port->tx_enabled = false;
    my_port->rx_enabled = false;
    my_port->break_enable = false;

    spin_unlock_irqrestore(&my_port->lock, flags);
    pr_info("my_uart: shutdown\n");//打印信息，告诉用户串口已经关闭
}

static void my_uart_set_termios(struct uart_port *port, struct ktermios *termios, struct ktermios *old)
{
    struct my_uart_port *my_port;
    unsigned long flags;
    unsigned int baud;
    unsigned int data_bits;
    unsigned int stop_bits;
    char parity;
    bool hw_flow_control;

    my_port = container_of(port, struct my_uart_port, port);

    /* 1. 获取波特率 */
    baud = uart_get_baud_rate(port, termios, old, 9600, 115200);//当前端口，从新的获取去修改旧的，若超出允许范围，则限制在 9600 ~ 115200 之间

    /* 2. 获取数据位 
    * 根据 c_cflag 中的 CSIZE 字段，
    * 解析当前串口使用的数据位（5/6/7/8 位）。
    */
    switch (termios->c_cflag & CSIZE) {
    case CS5:
        data_bits = 5;
        break;
    case CS6:
        data_bits = 6;
        break;
    case CS7:
        data_bits = 7;
        break;
    case CS8:
    default:
        data_bits = 8;
        break;
    }

    /*判断 termios 的 c_cflag 标志位中，CSTOPB 这一位有没有被置 1。
    termios 是一个结构体，包含了串口的各种配置参数，其中 c_cflag 是一个标志位字段，用于表示串口的控制模式。
    CSTOPB 是其中的一个标志位，用于指定停止位的数量。*/

    /* 3. 获取停止位 */
    if (termios->c_cflag & CSTOPB)//把 c_cflag 和 CSTOPB 做按位与，检查 STOP 这一位是不是 1
        stop_bits = 2;
    else
        stop_bits = 1;

    /* 4. 获取校验位 */
    if (termios->c_cflag & PARENB) // 把 c_cflag 和 PARENB 做按位与，检查 PARENB 这一位是不是 1
    {
        if (termios->c_cflag & PARODD)//把 c_cflag 和 PARODD 做按位与，检查 PARODD 这一位是不是 1               
            parity = 'O';     /* Odd 奇校验 */
        else
            parity = 'E';     /* Even 偶校验 */
    } else {
        parity = 'N';         /* None 无校验 */
    }

    /* 5. 获取硬件流控 */
    if (termios->c_cflag & CRTSCTS)//把 c_cflag 和 CSTOPB 做按位与，检查 STOP 这一位是不是 1
        hw_flow_control = true;//硬件流控
    else
        hw_flow_control = false;//软件流控

    spin_lock_irqsave(&my_port->lock, flags);

    my_port->baud = baud;//保存波特率
    my_port->data_bits = data_bits;//   保存数据位
    my_port->stop_bits = stop_bits;//保存停止位
    my_port->parity = parity;//保存校验位
    my_port->hw_flow_control = hw_flow_control;//保存硬件流控

    spin_unlock_irqrestore(&my_port->lock, flags);

    /* 6. 更新 serial_core 的超时时间 */
    uart_update_timeout(port, termios->c_cflag, baud);//波特率、数据位、停止位、校验位等参数的改变会影响数据传输的速率，从而影响超时时间的计算。serial_core 需要根据新的串口配置来重新计算超时时间，以确保数据传输的可靠性。
    pr_info("my_uart: set termios: %d %c %d %s %s\n", baud, parity, data_bits, stop_bits == 1 ? "1" : "2", hw_flow_control ? "hardware" : "none");
}

static void my_uart_config_port(struct uart_port *port, int flags)//告诉 Linux 内核，这个端口是一个标准的 16550 UART 类型，Linux 内核会根据这个类型来选择合适的驱动和操作方式。
{
    if (flags & UART_CONFIG_TYPE)
        port->type = PORT_16550;
}

static int my_uart_verify_port(struct uart_port *port, struct serial_struct *ser)//验证串口端口配置是否合法
{
    /*
    * 虚拟 UART 无真实 IRQ、IO 地址等硬件资源，
    * 当前仅允许未知类型或 16550 类型。
    */
   if (ser->type != PORT_UNKNOWN && ser->type != PORT_16550)//如果不是未知类型或 16550 类型，则返回错误码
        return -EINVAL;

    return 0;
}