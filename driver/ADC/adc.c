#include<linux/kernel.h>
#include<linux/module.h>
#include<linux/init.h>
#include<linux/clk.h>
#include<linux/cdev.h>
#include<linux/miscdevice.h>
#include<mach/map.h>

static void __iomem *vir_base_addr;
static struct clk *adc_clk;




static struct file_operations adc_fops = {
	.owner = THIS_MODULE,
	.open  = adc_open,
	.release = adc_release,
	.read = adc_read,
};



static struct miscdevice adc_misc ={
	.owner = THIS_MODULE,
	.name ="adc_test",
	.fops = &adc_fops,
};


static int __init adc_init()
{
	int ret;
	vir_base_addr = ioremap(adc_addr_start,ox20);
	if(!vir_base_addr)
	{
		printk(KERN_ERR"Failed to remap\n");
		return -ENOMEN;
	}
	adc_clk = clk_get(NULL,"ADC");
	if(!adc_clk)
	{
		printk(KERN_ERR"Failed to get adc clock\n");
		return -ENOENT;
	}
	clk_enable(adc_clk);
	ret = misc_register(&adc_misc);
	printk("adc_init return ret:%d\n",ret);
	return ret;
}
static void __void adc_exit()
{
	iounmap(vir_base_addr);
	if(adc_clk)
	{
		clk_disable(adc_clk);
		clk_put(adc_clk);
		adc_clk = NULL;
	}
	misc_deregister(&adc_misc);
}


module_init(adc_init);
module_exit(adc_exit);
MODULE_LICENSE("GPL");
