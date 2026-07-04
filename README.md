自定义标准Linux UART串口驱动开发需求文档（基于内核标准UART框架
 
一、总体规范
 
1. 严格遵循Linux内核标准串口子系统框架，使用 struct uart_driver 、 struct uart_port 、 struct uart_ops 整套标准UART驱动接口实现，不使用独立裸字符设备cdev自定义文件操作集，由内核tty层自动封装字符设备逻辑。
2. 驱动加载后内核自动生成标准tty串口设备节点： /dev/tty_my_uart ，归属内核tty子系统管理，可使用常规串口工具（stty、cat、echo、minicom）直接访问，同时配套自研用户态测试程序。
3. 双调试文件系统支持：
- 内核原生tty层默认ttyfs；
- 额外自建/proc调试节点，用于统计收发字节、查看端口状态、重置计数。
4. 增加全局流量统计模块：统计所有应用读写该串口的总读字节、总写字节，多进程并发访问数据累加计数。
 
二、标准UART驱动框架硬性要求
 
1. 核心UART结构体实现
 
1. 定义私有端口结构体，内嵌 struct uart_port 标准端口基类，扩展私有缓存、统计、锁、等待队列。
2. 实例化 struct uart_driver 全局串口驱动对象，注册至内核tty串口子系统，指定设备名前缀 tty_my_uart 。
3. 完整实现 struct uart_ops 全部标准回调接口，不能缺省：
- tx_empty：判断发送FIFO是否为空
- set_mctrl：设置调制解调器控制信号（CTS/RTS/DTR等）
- get_mctrl：读取调制解调器状态
- stop_tx / start_tx：启停发送通路
- stop_rx / start_rx：启停接收通路
- set_termios：配置串口波特率、数据位、校验、停止位、流控（termio标准参数）
- break_ctl：发送串口BREAK信号
- startup：串口打开时初始化硬件/虚拟缓存
- shutdown：串口关闭时释放资源
- config_port：端口基础初始化
- verify_port：校验termios配置合法性
 
2. 端口数据通路（虚拟模拟串口，无需真实硬件）
 
1. 虚拟TX/RX环形缓冲区模拟硬件FIFO，读写加自旋锁/互斥锁保护并发。
2. 内核tty层 uart_write 下发的数据存入TX缓存；提供内部回环逻辑（可选），TX数据自动转入RX缓存模拟收发。
3. 无接收数据时，tty读操作阻塞，配套wait_queue实现阻塞唤醒机制，符合标准串口阻塞读写行为。
4. 波特率、数据位、奇偶校验、硬件流控配置全部由 set_termios 接管，内核tty工具可正常配置参数。
 
3. 设备节点生成规则
 
1. 通过 uart_register_driver 注册标准UART驱动， uart_add_one_port 挂载自定义端口。
2. 内核tty子系统自动在/dev生成 tty_my_uartX （单端口仅 tty_my_uart ），设备由ttyfs管理，无需手动注册cdev、class、device。
3. 驱动卸载调用 uart_remove_one_port  +  uart_unregister_driver ，内核自动销毁/dev下tty设备节点。
 
三、proc文件系统功能要求
 
1. 创建/proc/myuart节点，采用seq_file接口实现安全读取。
2. 读取/proc/myuart输出内容：
- 串口驱动名称、端口编号
- 当前termios配置（波特率、校验、数据位）
- TX/RX缓存占用长度
- 累计总读取字节数、累计总写入字节数
- 调制控制信号状态（RTS/CTS/DCD等）
3. 支持向/proc/myuart写入 reset 命令，清零读写字节统计计数器；其他非法指令返回提示。
4. 模块加载创建proc节点，模块卸载自动删除proc节点，无内存泄漏。
 
四、读写字节统计模块要求
 
1. 内核侧维护两个64位全局统计变量：total_rx_bytes（应用读取总字节）、total_tx_bytes（应用写入总字节）。
2. 统计计数触发时机：
- 用户态write写入串口，数据拷贝至TX缓存后累加total_tx_bytes；
- 用户态read从RX缓存取出数据返回用户空间后累加total_rx_bytes。
3. 多进程、多线程并发读写时，统计变量使用原子变量保护，避免计数错乱。
4. 计数清零功能由proc文件写入命令触发。
 
五、用户态应用程序开发要求
 
1. 自研测试APP，标准Linux串口编程（调用tty标准API，open/read/write/ioctl操作/dev/tty_my_uart），不依赖私有字符设备ioctl。
2. APP功能清单：
1. 打开自定义串口设备，支持设置波特率、校验位、流控；
2. 循环写入指定长度测试数据，打印单次写入字节；
3. 阻塞读取串口返回数据，打印单次读取字节；
4. 读取/proc/myuart文件，解析并打印全局累计读写总字节；
5. 向/proc/myuart写入reset，清空内核侧统计计数；
6. 支持参数控制读写次数、单次数据长度。
3. APP适配标准tty串口行为，可与stty、echo等系统工具混用操作同一设备。
 
六、模块加载/卸载与资源约束要求
 
1. 模块入口：完成uart驱动注册、端口挂载、proc节点创建；
2. 模块出口：逆序销毁资源，删除proc、卸载uart端口、注销uart驱动；
3. 所有缓存、锁、等待队列在卸载时完整释放，无内存泄露、死锁；
4. 支持多进程同时打开 tty_my_uart ，端口缓存、统计计数全局共享；
5. 兼容内核标准串口调试工具，dmesg可打印uart端口注册、收发日志。
 
七、区分边界说明（重点）
 
1. 底层：完全依托内核 serial_core.c 标准UART框架，uart_driver/uart_port/uart_ops为核心，禁止单独创建struct cdev实现文件操作；
2. 上层设备：/dev/tty_my_uart由内核tty子系统自动生成，由ttyfs管理；
3. 扩展调试：额外新增proc文件系统作为独立统计调试入口，和标准UART框架解耦；
4. 数据统计：在内核uart读写通路埋点计数，全局统一统计所有用户层读写流量。
