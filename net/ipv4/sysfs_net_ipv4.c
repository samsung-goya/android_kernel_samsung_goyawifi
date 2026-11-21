// SPDX-License-Identifier: GPL-2.0
/*
 * sysfs-based networking knobs (/sys/kernel/ipv4/tcp_*) for Android
 * ConnectivityService TCP buffer size tuning, plus a minimal
 * /proc/net/xt_qtaguid/ctrl compat stub so Android 9's
 * NetworkManagementService enables eBPF bandwidth control on Linux 4.19.
 *
 * Copyright (C) 2008 Google, Inc.
 * Author: Robert Love <rlove@google.com>
 */

#include <linux/kobject.h>
#include <linux/init.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/string.h>
#include <linux/sysfs.h>
#include <net/net_namespace.h>
#include <net/tcp.h>

#define CREATE_IPV4_FILE(_name, _var) \
static ssize_t _name##_show(struct kobject *kobj, \
			    struct kobj_attribute *attr, char *buf) \
{ \
	return sprintf(buf, "%d\n", _var); \
} \
static ssize_t _name##_store(struct kobject *kobj, \
			     struct kobj_attribute *attr, \
			     const char *buf, size_t count) \
{ \
	int val, ret; \
	ret = kstrtoint(buf, 10, &val); \
	if (ret < 0 || val < 0) \
		return -EINVAL; \
	_var = val; \
	return count; \
} \
static struct kobj_attribute _name##_attr = \
	__ATTR(_name, 0644, _name##_show, _name##_store)

CREATE_IPV4_FILE(tcp_wmem_min, init_net.ipv4.sysctl_tcp_wmem[0]);
CREATE_IPV4_FILE(tcp_wmem_def, init_net.ipv4.sysctl_tcp_wmem[1]);
CREATE_IPV4_FILE(tcp_wmem_max, init_net.ipv4.sysctl_tcp_wmem[2]);

CREATE_IPV4_FILE(tcp_rmem_min, init_net.ipv4.sysctl_tcp_rmem[0]);
CREATE_IPV4_FILE(tcp_rmem_def, init_net.ipv4.sysctl_tcp_rmem[1]);
CREATE_IPV4_FILE(tcp_rmem_max, init_net.ipv4.sysctl_tcp_rmem[2]);

static struct attribute *ipv4_attrs[] = {
	&tcp_wmem_min_attr.attr,
	&tcp_wmem_def_attr.attr,
	&tcp_wmem_max_attr.attr,
	&tcp_rmem_min_attr.attr,
	&tcp_rmem_def_attr.attr,
	&tcp_rmem_max_attr.attr,
	NULL
};

static const struct attribute_group ipv4_attr_group = {
	.attrs = ipv4_attrs,
};

#ifdef CONFIG_PROC_FS
static int qtaguid_ctrl_show(struct seq_file *m, void *v)
{
	return 0;
}

static int qtaguid_ctrl_open(struct inode *inode, struct file *file)
{
	return single_open(file, qtaguid_ctrl_show, NULL);
}

static ssize_t qtaguid_ctrl_write(struct file *file, const char __user *buf,
				  size_t count, loff_t *ppos)
{
	return count;
}

static const struct file_operations qtaguid_ctrl_fops = {
	.open		= qtaguid_ctrl_open,
	.read		= seq_read,
	.write		= qtaguid_ctrl_write,
	.llseek		= seq_lseek,
	.release	= single_release,
};

static void __init qtaguid_compat_init(void)
{
	struct proc_dir_entry *dir;

	dir = proc_mkdir("xt_qtaguid", init_net.proc_net);
	if (dir)
		proc_create("ctrl", 0666, dir, &qtaguid_ctrl_fops);
}
#else
static inline void qtaguid_compat_init(void) {}
#endif

static __init int sysfs_ipv4_init(void)
{
	struct kobject *ipv4_kobject;
	int ret;

	qtaguid_compat_init();

	ipv4_kobject = kobject_create_and_add("ipv4", kernel_kobj);
	if (!ipv4_kobject)
		return -ENOMEM;

	ret = sysfs_create_group(ipv4_kobject, &ipv4_attr_group);
	if (ret) {
		kobject_put(ipv4_kobject);
		return ret;
	}

	return 0;
}

subsys_initcall(sysfs_ipv4_init);
