/******************************************************************************
 * @file    my_uart_core.c
 * @brief   基于 Linux UART/TTY（serial_core）框架的虚拟 UART 驱动核心层
 *
 * @author  李宇坤
 * @date    2026-07
 * @version V1.0
 *
 * @details
 * 本文件实现 UART 驱动的核心注册与框架对接逻辑，基于 Linux
 * serial_core UART 子系统实现，不使用独立字符设备（cdev）模型。
 *
 * 本模块主要职责：
 * 1. 注册并管理 struct uart_driver 实例；
 * 2. 初始化并绑定 struct uart_port 端口实例；
 * 3. 将 uart_port 挂载到 UART core（serial_core）框架；
 * 4. 完成驱动模块的加载与卸载流程管理；
 * 5. 由 Linux TTY 层自动创建 /dev/tty_my_uart0 设备节点；
 *
 * 本文件仅负责 UART 框架接入与资源管理，
 * 不实现 FIFO 数据通路、收发逻辑及 proc 调试接口，
 * 相关功能由 my_uart_port.c 与 my_uart_proc.c 实现。
 ******************************************************************************/

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/serial_core.h>
#include <linux/serial.h>
#include <linux/tty_flip.h>

#include "my_uart.h"

/*定义全局变量*/
struct my_uart_port my_uart_port;
extern const struct uart_ops my_uart_ops;

/*1. uart_driver */

/*这是整个 UART 驱动对象*/
struct uart_driver my_uart_driver = {
    .owner       = THIS_MODULE,//指定这个驱动属于哪个内核模块，不是自己定义的
    .driver_name = MY_UART_NAME,//UART 驱动名称
    .dev_name    = MY_UART_DEV_NAME,//TTY 设备名前缀
    .major       = 0,           //主设备号，0表示让内核自己分配设备号
    .minor       = 0,           //起始次设备号，从 0 开始
    .nr          = MY_UART_NR, //支持几个端口
};


/*2. moudule_init */

static int __init my_uart_init(void)
{
    int ret;

    /* 1. 注册 UART 驱动 */
    ret = uart_register_driver(&my_uart_driver);//uart_register_driver：注册整个 UART 驱动
    if (ret)
        return ret;

    /* 2. 初始化全局端口对象 */
    memset(&my_uart_port, 0, sizeof(my_uart_port)); //从my_uart_port开始填充，填充成0，填充多少字节

    spin_lock_init(&my_uart_port.lock);//自旋锁初始化函数，只有一个 CPU 可以修改 FIFO
    init_waitqueue_head(&my_uart_port.read_wait);//等待队列初始化函数，等待队列让进程堵塞

    atomic64_set(&my_uart_port.total_tx_bytes, 0);//把累积发送字节清为0
    atomic64_set(&my_uart_port.total_rx_bytes, 0);//把累积接受介字节清为0

    /* 3. 初始化 struct uart_port */
    my_uart_port.port.line = 0;//端口号
    my_uart_port.port.type = PORT_16550;//硬件类型
    my_uart_port.port.iotype = UPIO_MEM;//CPU 如何访问 UART
    my_uart_port.port.fifosize = MY_UART_FIFO_SIZE;//告诉 Linux：这个 UART FIFO 有多大
    my_uart_port.port.ops = &my_uart_ops;//最重要的成员，由谁操作，
    my_uart_port.port.flags = UPF_BOOT_AUTOCONF;//UART 的各种标志位，UPF_BOOT_AUTOCONF允许串口核心做基础自动配置
    my_uart_port.port.uartclk = 24000000;//UART 输入时钟

    /* 4. 把端口挂到 UART 驱动 */
    ret = uart_add_one_port(&my_uart_driver, &my_uart_port.port);//把一个 struct uart_port 挂到已经注册好的 struct uart_driver 上
    if (ret) {
        uart_unregister_driver(&my_uart_driver);
        return ret;
    }//如果挂载失败

    /* 5. 创建 /proc/myuart */
    ret = my_uart_proc_init();
    if (ret) {
        uart_remove_one_port(&my_uart_driver, &my_uart_port.port);//uart_remove_one_port：从 UART 驱动中移除一个 UART 端口
        uart_unregister_driver(&my_uart_driver);//uart_unregister_driver：注销整个 UART 驱动
        return ret;
    }

    pr_info("my_uart: init success\n");

    return 0;
}

/*3. moudule_exit */
static void __exit my_uart_exit(void)
{
    pr_info("my_uart: module exit\n");

    /* 1. 删除 /proc/myuart 节点 */
    my_uart_proc_exit();

    /* 2. 移除 UART 端口，自动删除 /dev/tty_my_uart0 */
    uart_remove_one_port(&my_uart_driver, &my_uart_port.port);

    /* 3. 注销 UART 驱动 */
    uart_unregister_driver(&my_uart_driver);

    pr_info("my_uart: module unloaded\n");
}

module_init(my_uart_init);
module_exit(my_uart_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("李宇坤");
MODULE_DESCRIPTION("Virtual UART Driver");