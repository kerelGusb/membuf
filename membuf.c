#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/device.h>
#include <linux/rwlock.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/uaccess.h>
#include <linux/ioctl.h>
#include <linux/mutex.h>
#include <linux/rwsem.h>

#define DEVICE_NAME "membuf"
#define CLASS_NAME  "membuf_class"

MODULE_LICENSE("GPL");
MODULE_AUTHOR("KDA");
MODULE_DESCRIPTION("memory buffer");

static DECLARE_RWSEM(devices_lock);

struct membuf_device {
    dev_t devnum;
    struct cdev membuf_cdev;
    struct device *memdev;
    char* buffer;
    size_t buf_size;
    size_t data_size;
    struct mutex buf_lock;
    int open_count;
    struct mutex open_count_lock;
};

static dev_t base_dev;
static struct class *membuf_class;

static struct file_operations fops;


#define MAX_DEVICES 128

static struct membuf_device devices[MAX_DEVICES];
static bool module_initialized = false;

// Module parameters configuration
static int DEVICE_NUM = 3;
static int INITIAL_BUF_SIZE = 1024;


static int set_device_num(const char *val, const struct kernel_param *kp)
{
    int new_num, i, ret = 0;
    int buffers_created = 0;
    int devices_created = 0;
    int cdevs_added = 0;
    
    if (kstrtoint(val, 10, &new_num) < 0 || new_num <= 0 || new_num > MAX_DEVICES)
    {
        pr_err("membuf: invalid device number parameter\n");
        return -EINVAL;
    }
    
    down_write(&devices_lock);

    // If DEVICE_NUM set before initialization, this code will throw an
    // error. So if this happens, we just changing DEVICE_NUM and do nothing.
    if (module_initialized) {
        if (new_num < DEVICE_NUM) {
            for (i = new_num; i < DEVICE_NUM; i++) {
                mutex_lock(&devices[i].open_count_lock);
                if (devices[i].open_count > 0) {
                    mutex_unlock(&devices[i].open_count_lock);
                    pr_err("membuf: device %d is busy\n", i);
                    ret = -EBUSY;
                    goto out;
                }
                mutex_unlock(&devices[i].open_count_lock);
            }
            for (i = new_num; i < DEVICE_NUM; i++) {
                kfree(devices[i].buffer);
                device_destroy(membuf_class, devices[i].devnum);
                cdev_del(&devices[i].membuf_cdev);
            }
        }
        if (new_num > DEVICE_NUM) {
            for (i = DEVICE_NUM; i < new_num; i++) {

                devices[i].devnum = MKDEV(MAJOR(base_dev), MINOR(base_dev) + i);

                cdev_init(&devices[i].membuf_cdev, &fops);
                devices[i].membuf_cdev.owner = THIS_MODULE;

                
                if (ret = cdev_add(&devices[i].membuf_cdev, devices[i].devnum, 1)) {
                    pr_err("membuf: cdev_add failed for device %d\n", i);
                    goto err_cdev_del;
                }
                cdevs_added++;

                devices[i].memdev = device_create(membuf_class, NULL, devices[i].devnum, NULL, DEVICE_NAME "%d", i);
                mutex_init(&devices[i].buf_lock);
                mutex_init(&devices[i].open_count_lock);

                if (IS_ERR(devices[i].memdev)) {
                    pr_err("membuf: device_create failed for device %d\n", i);
                    cdev_del(&devices[i].membuf_cdev);
                    ret = PTR_ERR(devices[i].memdev);
                    goto err_device_destroy;
                }
                devices_created++;


                devices[i].buffer = kzalloc(INITIAL_BUF_SIZE, GFP_KERNEL);
                if (!devices[i].buffer) {
                    pr_err("membuf: failed to allocate buffer for device %d\n", i);
                    ret = -ENOMEM;
                    goto err_alloc_fail;
                }

                devices[i].buf_size = INITIAL_BUF_SIZE;
                devices[i].data_size = 0;
                devices[i].open_count = 0;
                buffers_created++;
            }
        }


        *(int *)kp->arg = new_num;
        up_write(&devices_lock);
        pr_info("membuf: device number set to %d\n", new_num);
        return 0;


err_alloc_fail:
        for (i = 0; i < buffers_created; ++i) {
            kfree(devices[i].buffer);
        }

err_device_destroy:
        for (i = 0; i < devices_created; ++i) {
            device_destroy(membuf_class, devices[i].devnum);
        }
        class_destroy(membuf_class);

err_cdev_del:
        for (i = 0; i < cdevs_added; ++i) {
            cdev_del(&devices[i].membuf_cdev);
        }
    } else {
        *(int *)kp->arg = new_num;
        pr_info("membuf: device number set to %d\n", new_num);
    }

out:
    up_write(&devices_lock);

    return ret;
}


static const struct kernel_param_ops device_num_ops = {
    .set = set_device_num,
    .get = param_get_int
};

module_param_cb(DEVICE_NUM, &device_num_ops, &DEVICE_NUM, S_IRUGO | S_IWUSR);
MODULE_PARM_DESC(DEVICE_NUM, "Number of devices");


static int set_buf_size(const char *val, const struct kernel_param *kp)
{
    int new_size;

    if (kstrtoint(val, 10, &new_size) < 0 || new_size <= 0) {
        pr_err("membuf: invalid buffer size parameter\n");
        return -EINVAL;
    }

    *(int *)kp->arg = new_size;
    pr_info("membuf: buffer size set to %d bytes\n", new_size);

    return 0;
}

static const struct kernel_param_ops buf_size_ops = {
    .set = set_buf_size,
    .get = param_get_int,
};

module_param_cb(INITIAL_BUF_SIZE, &buf_size_ops, &INITIAL_BUF_SIZE, S_IRUGO | S_IWUSR);
MODULE_PARM_DESC(INITIAL_BUF_SIZE, "Initial size of the memory buffer in bytes");


#define MEMBUF_IOC_MAGIC 'm'
#define MEMBUF_IOC_RESIZE _IOW(MEMBUF_IOC_MAGIC, 1, struct membuf_resize)

struct membuf_resize {
    unsigned int minor;
    size_t new_size;
};

static long membuf_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
    struct membuf_device *dev = file->private_data;

    if (_IOC_TYPE(cmd) != MEMBUF_IOC_MAGIC) {
        return -ENOTTY;
    }

    if (cmd != MEMBUF_IOC_RESIZE) return -ENOTTY;

    struct membuf_resize resize_info;
    if (copy_from_user(&resize_info, (void __user *)arg, sizeof(resize_info))) {
        return -EFAULT;
    }
    if (resize_info.minor != iminor(file->f_inode)) {
        return -EINVAL;
    }

    mutex_lock(&dev->buf_lock);
    char *new_buffer = kmalloc(resize_info.new_size, GFP_KERNEL);
    if (!new_buffer) {
        mutex_unlock(&dev->buf_lock);
        return -ENOMEM;
    }
    size_t copy_size = (resize_info.new_size < dev->buf_size) ? resize_info.new_size : dev->buf_size;
    memcpy(new_buffer, dev->buffer, copy_size);
    kfree(dev->buffer);
    dev->buffer = new_buffer;
    dev->buf_size = resize_info.new_size;

    if (dev->data_size > dev->buf_size)
        dev->data_size = dev->buf_size;

    mutex_unlock(&dev->buf_lock);

    pr_info("membuf: resized buffer for device %d to %zu bytes\n", resize_info.minor, resize_info.new_size);
    
    return 0;
}

static int membuf_open(struct inode *inode, struct file *file)
{
    down_read(&devices_lock);

    int minor = iminor(inode);

    if (minor >= DEVICE_NUM) {
        up_read(&devices_lock);
        return -ENODEV;
    }

    mutex_lock(&devices[minor].open_count_lock);
    devices[minor].open_count++;
    mutex_unlock(&devices[minor].open_count_lock);

    file->private_data = &devices[minor];

    pr_info("membuf: opened by process %d (%s)\n", 
            current->pid, current->comm);

    up_read(&devices_lock);

    return 0;
}


static int membuf_release(struct inode *inode, struct file *file)
{
    struct membuf_device *dev = file->private_data;

    mutex_lock(&dev->open_count_lock);
    dev->open_count--;
    mutex_unlock(&dev->open_count_lock);
    
    pr_info("membuf: closed by process %d (%s)\n", 
            current->pid, current->comm);

    return 0;
}


static ssize_t membuf_read(struct file *file, char __user *buffer, 
                           size_t length, loff_t *offset)
{
    struct membuf_device *dev = file->private_data;
    ssize_t bytes_read = 0;
    size_t chunk_size;
    size_t bytes_not_copied;
    size_t bytes_remaining;

    mutex_lock(&dev->buf_lock);

    pr_info("membuf: read attempt by process %d (%s), requested %zu bytes\n", 
            current->pid, current->comm, length);
    
    if (*offset >= dev->data_size) {
        mutex_unlock(&dev->buf_lock);
        pr_info("membuf: end of file reached (offset=%lld, size=%zu)\n", 
                *offset, dev->buf_size);
        return 0;
    }

    if (length > dev->data_size - *offset)
        bytes_remaining = dev->data_size - *offset;
    else
        bytes_remaining = length;
    
    if (bytes_remaining == 0) {
        mutex_unlock(&dev->buf_lock);
        return 0;
    }

    while (bytes_remaining > 0) {
        chunk_size = (bytes_remaining > 1024) ? 1024 : bytes_remaining;
        
        bytes_not_copied = copy_to_user(buffer + bytes_read, 
                                        dev->buffer + *offset + bytes_read, 
                                        chunk_size);
        
        if (bytes_not_copied > 0) {
            pr_warn("membuf: failed to copy %zu bytes to user at offset %lld\n", 
                    bytes_not_copied, *offset + bytes_read);
            
            if (bytes_not_copied == chunk_size) {
                if (bytes_read == 0) {
                    mutex_unlock(&dev->buf_lock);
                    return -EFAULT;
                }
                break;
            }
            
            bytes_read += (chunk_size - bytes_not_copied);
            break;
        }
        
        bytes_read += chunk_size;
        bytes_remaining -= chunk_size;
    }

    *offset += bytes_read;

    mutex_unlock(&dev->buf_lock);

    pr_info("membuf: read %zu bytes, new offset=%lld\n", bytes_read, *offset);

    return bytes_read;
}


static ssize_t membuf_write(struct file *file, const char __user *buffer,
                            size_t length, loff_t *offset)
{
    struct membuf_device *dev = file->private_data;
    ssize_t bytes_written = 0;
    size_t chunk_size;
    size_t bytes_not_copied;
    size_t bytes_remaining;

    mutex_lock(&dev->buf_lock);

    pr_info("membuf: write attempt by process %d (%s), %zu bytes, offset=%lld\n",
            current->pid, current->comm, length, *offset);	

    if (*offset == 0)
        dev->data_size = 0;

    if (*offset >= dev->buf_size) {
        mutex_unlock(&dev->buf_lock);
        pr_info("membuf: write beyond buffer size (offset=%lld, size=%zu)\n", 
                *offset, dev->buf_size);
        return -ENOSPC;
    }

    if (length > dev->buf_size - *offset)
        bytes_remaining = dev->buf_size - *offset;
    else
        bytes_remaining = length;

    if (bytes_remaining == 0) {
        mutex_unlock(&dev->buf_lock);
        return 0;
    }

    while (bytes_remaining > 0) {
        chunk_size = (bytes_remaining > 1024) ? 1024 : bytes_remaining;
        
        bytes_not_copied = copy_from_user(dev->buffer + *offset + bytes_written, 
                                         buffer + bytes_written, 
                                         chunk_size);
        
        if (bytes_not_copied > 0) {
            pr_warn("membuf: failed to copy %zu bytes from user at offset %lld\n", 
                    bytes_not_copied, *offset + bytes_written);
            
            if (bytes_not_copied == chunk_size) {
                if (bytes_written == 0) {
                    mutex_unlock(&dev->buf_lock);
                    return -EFAULT;
                }
                break;
            }
            
            bytes_written += (chunk_size - bytes_not_copied);
            break;
        }
        
        bytes_written += chunk_size;
        bytes_remaining -= chunk_size;
    }

    *offset += bytes_written;

    if (*offset > dev->data_size)
        dev->data_size = *offset;

    mutex_unlock(&dev->buf_lock);

    pr_info("membuf: wrote %zu bytes, new offset=%lld\n", bytes_written, *offset);
    
    return bytes_written;
}


static struct file_operations fops = {
    .owner = THIS_MODULE,
    .open = membuf_open,
    .release = membuf_release,
    .read = membuf_read,
    .write = membuf_write,
    .unlocked_ioctl = membuf_ioctl,
    .llseek = default_llseek,
};


static int membuf_uevent(const struct device *dev, struct kobj_uevent_env *env)
{
    add_uevent_var(env, "DEVMODE=%#o", 0666);
    return 0;
}


static int __init membuf_init(void)
{
    int ret, i;
    int buffers_created = 0;
    int devices_created = 0;
    int cdevs_added = 0;

    if ((ret = alloc_chrdev_region(&base_dev, 0, DEVICE_NUM, DEVICE_NAME))) {
        pr_err("membuf: failed to allocate device numbers\n");
        return ret;
    }

    pr_info("membuf: registered (major=%d, minors=%d-%d)\n", MAJOR(base_dev), MINOR(base_dev), MINOR(base_dev) + DEVICE_NUM - 1);

    for (i = 0; i < DEVICE_NUM; i++) {
        devices[i].devnum = MKDEV(MAJOR(base_dev), MINOR(base_dev) + i);

        cdev_init(&devices[i].membuf_cdev, &fops);
        devices[i].membuf_cdev.owner = THIS_MODULE;

        if (ret = cdev_add(&devices[i].membuf_cdev, devices[i].devnum, 1)) {
            pr_err("membuf: cdev_add failed for device %d\n", i);
            goto err_cdev_del;
        }
        cdevs_added++;
    }

    if (IS_ERR(membuf_class = class_create(CLASS_NAME))) {
        pr_err("membuf: class_create failed\n");
        ret = PTR_ERR(membuf_class);
        goto err_cdev_del;
    }

    membuf_class->dev_uevent = membuf_uevent;

    for (i = 0; i < DEVICE_NUM; ++i) {
        devices[i].memdev = device_create(membuf_class, NULL, devices[i].devnum, NULL, DEVICE_NAME "%d", i);
        mutex_init(&devices[i].buf_lock);
        mutex_init(&devices[i].open_count_lock);

        if (IS_ERR(devices[i].memdev)) {
            pr_err("membuf: device_create failed for device %d\n", i);
            ret = PTR_ERR(devices[i].memdev);
            goto err_device_destroy;
        }
        devices_created++;
    }

    for (i = 0; i < DEVICE_NUM; ++i) {
        devices[i].buffer = kzalloc(INITIAL_BUF_SIZE, GFP_KERNEL);
        if (!devices[i].buffer) {
            pr_err("membuf: failed to allocate buffer for device %d\n", i);
            ret = -ENOMEM;
            goto err_alloc_fail;
        }
        devices[i].buf_size = INITIAL_BUF_SIZE;
        devices[i].data_size = 0;
        devices[i].open_count = 0;
        buffers_created++;
    }

    module_initialized = true;

    pr_info("membuf: module loaded\n");
    return 0;

err_alloc_fail:
    for (i = 0; i < buffers_created; ++i) {
        kfree(devices[i].buffer);
    }

err_device_destroy:
    for (i = 0; i < devices_created; ++i) {
        device_destroy(membuf_class, devices[i].devnum);
    }
    class_destroy(membuf_class);

err_cdev_del:
    for (i = 0; i < cdevs_added; ++i) {
        cdev_del(&devices[i].membuf_cdev);
    }

    unregister_chrdev_region(base_dev, DEVICE_NUM);
    return ret;
}


static void __exit membuf_exit(void)
{
    int i;
    
    for (i = 0; i < DEVICE_NUM; ++i) {
        device_destroy(membuf_class, devices[i].devnum);
        cdev_del(&devices[i].membuf_cdev);
        kfree(devices[i].buffer);
    }
    class_destroy(membuf_class);
    unregister_chrdev_region(base_dev, DEVICE_NUM);

    pr_info("membuf: module unloaded\n");
}


module_init(membuf_init);
module_exit(membuf_exit);