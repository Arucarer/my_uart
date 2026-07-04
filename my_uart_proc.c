#include <linux/module.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/uaccess.h>

#include "my_uart.h"

#define MY_UART_PROC_NAME "myuart"

static struct proc_dir_entry *my_uart_proc_entry;//就是内核表示proc对象


static int my_uart_proc_show(struct seq_file *m, void *v);
static int my_uart_proc_open(struct inode *inode, struct file *file);
static ssize_t my_uart_proc_write(struct file *file, const char __user *buf, size_t count, loff_t *ppos);

static const struct proc_ops my_uart_proc_ops = {
    .proc_open    = my_uart_proc_open,
    .proc_read    = seq_read,
    .proc_write   = my_uart_proc_write,
    .proc_lseek   = seq_lseek,
    .proc_release = single_release,
};


static int my_uart_proc_show(struct seq_file *m, void *v)
{
    
    struct my_uart_port *my_port = &my_uart_port;
    //准备临时变量把内核的数据取出来
    unsigned long flags;
    unsigned int tx_count;
    unsigned int rx_count;
    unsigned int mctrl;
    unsigned int baud;
    unsigned int data_bits;
    unsigned int stop_bits;
    char parity;

    //上锁
    spin_lock_irqsave(&my_port->lock, flags);

    tx_count = (my_port->tx_head - my_port->tx_tail) & (MY_UART_FIFO_SIZE - 1);
    rx_count = (my_port->rx_head - my_port->rx_tail) & (MY_UART_FIFO_SIZE - 1);
    mctrl = my_port->mctrl;
    baud = my_port->baud;
    data_bits = my_port->data_bits;
    stop_bits = my_port->stop_bits;
    parity = my_port->parity;

    spin_unlock_irqrestore(&my_port->lock, flags);

    //输出，往 proc 文件里面写
    seq_printf(m, "driver: %s\n", MY_UART_NAME);
    seq_printf(m, "dev_name: %s0\n", MY_UART_DEV_NAME);
    seq_printf(m, "port_line: %u\n", my_port->port.line);
    seq_printf(m, "baud: %u\n", baud);
    seq_printf(m, "data_bits: %u\n", data_bits);
    seq_printf(m, "parity: %c\n", parity);
    seq_printf(m, "stop_bits: %u\n", stop_bits);
    seq_printf(m, "tx_fifo_used: %u\n", tx_count);
    seq_printf(m, "rx_fifo_used: %u\n", rx_count);
    seq_printf(m, "total_tx_bytes: %lld\n",
               atomic64_read(&my_port->total_tx_bytes));
    seq_printf(m, "total_rx_bytes: %lld\n",
               atomic64_read(&my_port->total_rx_bytes));
    seq_printf(m, "mctrl: 0x%x\n", mctrl);

    return 0;
}

static int my_uart_proc_open(struct inode *inode, struct file *file)
{
    return single_open(file, my_uart_proc_show, NULL);
}
//proc 文件已经被打开了，以后如果用户 read 它，就调用 my_uart_proc_show() 来生成内容


static ssize_t my_uart_proc_write(struct file *file, const char __user *buf, size_t count, loff_t *ppos)
{
    char cmd[32];//缓冲区

    if (count >= sizeof(cmd))//避免越界
        return -EINVAL;

    if (copy_from_user(cmd, buf, count))//如果全部返回，则为0，没有全部返回就是1，就会进入
        return -EFAULT;

    cmd[count] = '\0';//字符串结束符

    if (!strncmp(cmd, "reset", 5)) //如果输入的是reset就...
    {
        atomic64_set(&my_uart_port.total_tx_bytes, 0);
        atomic64_set(&my_uart_port.total_rx_bytes, 0);

        pr_info("my_uart: proc reset counters\n");//输出proc reset counters
        return count;
    }

    pr_info("my_uart: invalid proc command: %s\n", cmd);//  输入的命令无效
    return -EINVAL;//返回无效参数
}



int my_uart_proc_init(void)
{
    my_uart_proc_entry = proc_create(MY_UART_PROC_NAME, 0666, NULL, &my_uart_proc_ops);
    if (!my_uart_proc_entry)//如果创建失败
        return -ENOMEM;

    pr_info("my_uart: /proc/%s created\n", MY_UART_PROC_NAME);
    return 0;
}

void my_uart_proc_exit(void)
{
    if (my_uart_proc_entry) //如果 proc 文件存在
    {
        proc_remove(my_uart_proc_entry);
        my_uart_proc_entry = NULL;
    }

    pr_info("my_uart: /proc/%s removed\n", MY_UART_PROC_NAME);//删除 proc 文件
}