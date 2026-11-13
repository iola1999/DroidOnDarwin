/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/net.h>
#include <linux/nsproxy.h>
#include <linux/poll.h>
#include <linux/sched.h>
#include <linux/socket.h>
#include <linux/uio.h>
#include <asm/nce-graphics-socket-address.h>

static int nce_graphics_pipe_open(struct inode *inode, struct file *file)
{
    struct nce_graphics_socket_address address = NCE_GRAPHICS_SOCKET_ADDRESS_INIT;
    struct socket *socket;
    int error;

    error = sock_create_kern(current->nsproxy->net_ns, AF_UNIX, SOCK_STREAM, 0, &socket);
    if (error)
        return error;
    /* misc_open 持有全局 misc_mtx，连接不得等待 listener backlog。 */
    error = kernel_connect(socket, (struct sockaddr *)&address, NCE_GRAPHICS_SOCKET_ADDRESS_BYTES, O_NONBLOCK);
    if (error) {
        sock_release(socket);
        return error;
    }

    /* stream_open 取消文件位置串行化，使 read/write 可并发等待。 */
    stream_open(inode, file);
    file->f_mode |= FMODE_NOWAIT;
    file->private_data = socket;
    return 0;
}

static ssize_t nce_graphics_pipe_read_iter(struct kiocb *iocb, struct iov_iter *to)
{
    struct file *file = iocb->ki_filp;
    struct socket *socket = file->private_data;
    struct msghdr message = {.msg_iter = *to, .msg_iocb = iocb};
    ssize_t count;

    if (iocb->ki_pos != 0)
        return -ESPIPE;
    if (!iov_iter_count(to))
        return 0;
    if (file->f_flags & O_NONBLOCK || iocb->ki_flags & IOCB_NOWAIT)
        message.msg_flags = MSG_DONTWAIT;
    count = sock_recvmsg(socket, &message, message.msg_flags);
    *to = message.msg_iter;
    /* 原 qemu_pipe_read_fully 的非空读取遇到 0 会继续循环；关闭按原设备返回 -EIO。 */
    return count ? count : -EIO;
}

static ssize_t nce_graphics_pipe_write_iter(struct kiocb *iocb, struct iov_iter *from)
{
    struct file *file = iocb->ki_filp;
    struct socket *socket = file->private_data;
    struct msghdr message = {.msg_iter = *from, .msg_iocb = iocb, .msg_flags = MSG_NOSIGNAL};
    ssize_t count;

    if (iocb->ki_pos != 0)
        return -ESPIPE;
    if (!iov_iter_count(from))
        return 0;
    if (file->f_flags & O_NONBLOCK || iocb->ki_flags & IOCB_NOWAIT)
        message.msg_flags |= MSG_DONTWAIT;
    count = sock_sendmsg(socket, &message);
    *from = message.msg_iter;
    return count;
}

static __poll_t nce_graphics_pipe_poll(struct file *file, poll_table *wait)
{
    struct socket *socket = file->private_data;

    return socket->ops->poll(file, socket, wait);
}

static int nce_graphics_pipe_release(struct inode *inode, struct file *file)
{
    /* dup/fork 共享同一 file，VFS 仅在最后一个文件引用释放后调用 release。 */
    sock_release(file->private_data);
    return 0;
}

static const struct file_operations nce_graphics_pipe_fops = {
    .owner = THIS_MODULE,
    .open = nce_graphics_pipe_open,
    .read_iter = nce_graphics_pipe_read_iter,
    .write_iter = nce_graphics_pipe_write_iter,
    .poll = nce_graphics_pipe_poll,
    .release = nce_graphics_pipe_release,
};

static struct miscdevice nce_graphics_pipe = {
    .minor = MISC_DYNAMIC_MINOR,
    .name = "goldfish_pipe_dprctd",
    .fops = &nce_graphics_pipe_fops,
};

static int __init nce_graphics_pipe_init(void)
{
    return misc_register(&nce_graphics_pipe);
}
device_initcall(nce_graphics_pipe_init);
