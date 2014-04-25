#include<linux/i2c.h>
#include<linux/input.h>
#include<linux/interrupt.h>
#include<linux/irq.h>
#include<linux/io.h>
#include<linux/gpio.h>
#include<linux/input/mt.h>
#include<linux/delay.h>
#include<linux/nwd-config.h>
#include<linux/delay.h>
#include<mach/gpio.h>
#include<mach/nwd-gpio.h>


#define I2C_TP_NAME		"gslx680"
#define I2C_DEV_ADDR	0x40
#define IRQ_NUM			IRQ_CTP_INT

#define GSL_DATA_REG    0x80
#define GSL_STATUS_REG  0xe0
#define GSL_PAGE_REG    0xf0

#define PRESS_MAX		255
#define MAX_CONTACTS    10
#define MAX_FINGERS     10 
#define DMA_TRANS_LEN   0x20

struct gsl_ts_data{
	u8 x_index;
	u8 y_index;
	u8 z_index;
	u8 id_index;
	u8 touch_index;
	u8 data_reg;
	u8 status_reg;
	u8 data_size;
	u8 touch_bytes;
	u8 update_data;
	u8 touch_meta_data;
	u8 finger_size;
};

static struct gsl_ts_data devices[] = {
	{
		 .x_index = 6, 
		 .y_index = 4, 
		 .z_index = 5, 
		 .id_index = 7, 
		 .data_reg = GSL_DATA_REG,
		 .status_reg = GSL_STATUS_REG,
		 .update_data = 0x4, 
		 .touch_bytes = 4, 
		 .touch_meta_data = 4, 
		 .finger_size = 70,
	},
};

struct gsl_ts{
	struct i2c_client *client;
	struct input_dev *input;
	struct work_struct work;
	struct workqueue_struct *wq;
	struct gsl_ts_data *obj_data;
	u8 *touch_data;
	u8 device_id;
	int irq;
};


static u32 id_sign[MAX_CONTACTS+1] = {0};
static u8 id_state_flag[MAX_CONTACTS+1] = {0};
static u8 id_state_old_flag[MAX_CONTACTS+1] = {0};
static u16 x_old[MAX_CONTACTS+1] = {0};
static u16 y_old[MAX_CONTACTS+1] = {0};
static u16 x_new = 0;
static u16 y_new = 0;


static inline void	get_low_byte(u8 *buf,const u32 *fw)
{
	u32 *temp = (int *)buf;
	*temp = *fw;
}

static inline u16 bytes_merge(u8 a,u8,b)
{
	u16 ab = 0;
	ab = ab | a;
	ab = ab << 8 | b;
	return ab;
}

static int gslx680_i2c_read(struct i2c_client *client,const u8 addr_reg,u8 *buf,u32 len)
{
	struct i2c_msg msg[2];

	msg[0].addr = client->addr;
	msg[0].flags = client->flags & I2C_M_TEN;
	msg[0].len = 1; 
	msg[0].buf = &addr_reg;

	msg[1].addr = client->addr;
	msg[1].flags |= I2C_M_RD; 
	msg[1].len = len;
	msg[1].buf = buf;

	if(addr_reg < 0x80)
	{
		i2c_transfer(client->adapter,msg,ARRAY_SIZE(MSG));	
		msleep(5);
	}
					 
	return i2c_transfer(client->adapter, msg, ARRAY_SIZE(msg)) == ARRAY_SIZE(msg) ? 0 : -EFAULT;   
}

static int gslx680_i2c_write(struct i2c_client *client,const u8 addr_reg,u8 *buf,u32 len)
{
	struct i2c_msg msg[1];

	buf[0] = addr_reg;
	
	msg[0].addr = client->addr;
	msg[0].flags = client->flags & I2C_M_TEN;
	msg[0].len = len + 1;
	msg[0].buf = buf;

	return i2c_transfer(client->adapter,msg,1) == 1 ? 0 : -EFAULT;
}

static int read_chip_reg(struct i2c_client *client,u8 addr,u8 *pdata,int len)
{
	int ret;

	if(datalen > 126)
	{
		printk("%s too big len = %d\n",__func__,len);
		return -1;
	}
	ret = write_chip_reg(client,addr,NULL,0);
	if(ret < 0)
	{
		printk("%s set data address fail!\n",__func__);
		return ret;
	}
	ret = gslx680_i2c_read(client,addr,buf,datalen);
	return ret;	
}
static int write_chip_reg(struct i2c_client *client,u8 addr,u8 *pdata,int len)
{
	int ret;
	u8 buf[128] = {0};
	unsigned int datalen = 0;
	if(len > 125)
	{
		printk("%s too big len =%d\n",__func__,len);
		return -1;
	}
	//buf[0] = addr;
	datalen++;
	if(len != 0 && pdata != NULL)
	{
		memcpy(&buf[datalen],pdata,len);
		datalen += len;
	}
	ret = gslx680_i2c_write(client,addr,buf,datalen);
	//ret = i2c_master_send(client,buf,datalen);
	return ret;	
}

static void load_chip_fw(struct i2c_client *client)
{
	u8 buf[DMA_TRANS_LEN*4 + 1] = {0};
	u8 send_flag = 1;
	u8 *cur = buf + 1;
	u32 source_line = 0;
	u32 source_len;
	struct fw_data *ptr_fw;

	ptr_fw = GSLX680_FW;
	source_len =ARRAY_SIZE(GSLX680_FW);

	for(source_line = 0;source_line < source_len;source_line++)
	{
		if(GSL_PAGE_REG == ptr_fw[source_line].offset)
		{
			get_low_byte(cur,&ptr_fw[source_line].val);
			gslx680_i2c_write(client,GSL_PAGE_REG,buf,4);
			send_flag = 1;
		}
		else
		{
			if(1 == send_flag % (DMA_TRANS_LEN < 0X20 ? DMA_TRANS_LEN : 0x20))
				buf[0] = (u8)ptr_fw[source_line].offset;
			get_low_byte(cur,&ptr_fw[source_line].val);
			cur += 4;
			
			if(0 == send_flag % (DMA_TRANS_LEN < 0x20 ? DMA_TRANS_LEN : 0x20))
			{
				gslx680_i2c_write(client,buf[0],buf,cur - buf -1);
				cur = buf + 1;
			}
			send_flag++;
		}
	}
}

static void gslx680_chip_reset(struct i2c_client *client)
{
	u8 cmd = 0x88;
	u8 buf[4] = {0x00};

	write_chip_reg(client,0xe0,&cmd,sizeof(cmd));
	msleep(20);
	cmd = 0x04;
	write_chip_reg(client,0xe4,&cmd,sizeof(cmd));
	msleep(10);
	write_chip_reg(client,0xbc,buf,sizeof(buf));
	msleep(10);
}

static void gslx680_chip_startup(struct i2c_client *client)
{
	u8 cmd = 0x00;
	write_chip_reg(client,0xe0,&cmd,1);
	msleep(10);
}



static void clr_chip_reg(struct i2c_client *client)
{
	u8 buf[4] = {0};

	buf[0] = 0x88;
	write_chip_reg(client,0xe0,&buf[0],1);
	msleep(20);
	buf[0] = 0x03;
	write_chip_reg(client,0x80,&buf[0],1);
	msleep(20);
	buf[0] = 0x04;
	write_chip_reg(client,0xe4,&buf[0],1);
	msleep(20);
	buf[0] = 0x00;
	write_chip_reg(client,0xe0,&buf[0],1);
	msleep(20);
}



static int board_gpio_init(void)
{
	gpio_request(GPIO_CTP_RST,"gsl_rest");
	gpio_direction_output(GPIO_CTP_RST,0);
	mdelay(50);
	gpio_direction_output(GPIO_CTP_RST,1);
	gpio_request(GPIO_CTP_INT,"gsl_int");
	gpio_direction_input(GPIO_CTP_INT);
	gpio_pull(GPIO_CTP_INT,0);
	return 0;
}

static void gslx680_chip_init(struct i2c_client *client)
{
	int ret;

	gpio_direction_output(GPIO_CTP_RST,0);
	msleep(20);
	gpip_direction_output(GPIO_CTP_RST,1);
	msleep(20);

	if(test_i2c(client) <0 )
	{
		printk("------gslx680 i2c test error------\n");
		return;
	}
	
	clr_chip_reg(client);
	gslx680_chip_reset(client);
	load_chip_fw(client);
	gslx680_chip_startup(client);
	gslx680_chip_reset(client);
	gslx680_chip_startup(client);
	
}

static void record_point(u16 x,u16 y,u8 id)
{
	u16 x_err =0;
	u16 y_err =0;

	id_sign[id]=id_sign[id]+1;
	
	if(id_sign[id]==1)
	{
		x_old[id]=x;
		y_old[id]=y;
	}

	x = (x_old[id] + x)/2;
	y = (y_old[id] + y)/2;
		
	if(x>x_old[id])
		x_err=x -x_old[id];
	else
		x_err=x_old[id]-x;

	if(y>y_old[id])
		y_err=y -y_old[id];
	else
		y_err=y_old[id]-y;

	if( (x_err > 3 && y_err > 1) || (x_err > 1 && y_err > 3) )
	{
		x_new = x;     x_old[id] = x;
		y_new = y;     y_old[id] = y;
	}
	else
	{
		if(x_err > 3)
		{
			x_new = x;     
			x_old[id] = x;
		}
		else
			x_new = x_old[id];
		if(y_err> 3)
		{
			y_new = y;     
			y_old[id] = y;
		}
		else
			y_new = y_old[id];
	}

	if(id_sign[id]==1)
	{
		x_new= x_old[id];
		y_new= y_old[id];
	}
}


static void report_data(struct gsl_ts *ts,u16 x,u16 y,u8 press,u8 id)
{
	printk("----------report data---------\n");
	printk("==========id = %d,x = %d,y = %d==========\n",id,x,y);
	if(x > SCREEN_MAX_X || y > SCREEN_MAX_Y)
		return;
	input_mt_slot(ts->input,id);
	input_report_abs(ts->input,ABS_MT_TRACKING_ID,id);
	input_report_abs(ts->input,ABS_MT_POSITION_X,x);
	input_report_abs(ts->input,ABS_MT_POSITION_Y,y);
	input_report_abs(ts->input,ABS_MT_PRESSURE,press);
	input_report_abs(ts->input,BTN_TOUCH,1);
}

static void gslx680_ts_workfunc(struct work_struct *work)
{
	int ret,i;
	u8 id,touchs,buf[4] = {0};
	u16 x,y;

	struct gsl_ts *ts = container_of(work,struct gsl_ts,work);
	
	printk("==========gslx680_ts_workfunc========\n");

	ret = read_chip_reg(ts->client,0x80,ts->touch_data,ts->obj_data->data_size);
	if(ret < 0)
	{
		printk("in workfunc read data register failed.\n");
		enable_irq(ts->irq);
		return;
	}

	touchs = ts->touch_data[ts->obj_data->touch_index];
	printk("---------touchs:The %d .----------",touchs);

	for(i = 1;i <= MAX_CONTACTS;i++)
	{
		if(touchs == 0)
			id_sign[i] = 0;
		id_state_flag[i] = 0;
	}

	for(i = 0;i < (touchs > MAX_FINGERS ? MAX_FINGERS : touchs);i++)
	{
		x = bytes_merge((ts->touch_data[ts->obj_data->x_index + 4*i +1] & 0xf),ts->touch_data[ts->obj_data->x_index + 4 * i ]);	
		y = bytes_merge((ts->touch_data[ts->obj_data->y_index + 4 * i + 1] & 0xf),ts->touch_data[ts->obj_data->y_index + 4 * i]);
		id = ts->touch_data[ts->obj_data->id_index + 4 * i] >> 4;
		if(1 <= id && id <= MAX_CONTACTS)	
		{		
			record_point(x,y,id);
			report_data(ts,x_new,y_new,10,id);
			id_state_flag[id] = 1;
		}
	for(i = 1; i <= MAX_CONTACTS; i ++)
	{	
		if( (0 == touches) || ((0 != id_state_old_flag[i]) && (0 == id_state_flag[i])) )
		{
		#ifdef REPORT_DATA_ANDROID_4_0
			input_mt_slot(ts->input, i);
			input_report_abs(ts->input, ABS_MT_TRACKING_ID, -1);
			input_mt_report_slot_state(ts->input, MT_TOOL_FINGER, false);
		#endif
			id_sign[i]=0;
		}
		id_state_old_flag[i] = id_state_flag[i];
	}
	#ifndef REPORT_DATA_ANDROID_4_0
	if(0 == touches)
	{	
		input_mt_sync(ts->input);
	#ifdef HAVE_TOUCH_KEY
		if(key_state_flag)
		{
        	input_report_key(ts->input, key, 0);
			input_sync(ts->input);
			key_state_flag = 0;
		}
	#endif			
	}
	#endif
	input_sync(ts->input);
	enable_irq(ts->irq);
}


static int source_init(struct i2c_client *client,struct gsl_ts  *ts)
{
	int i,ret = 0;
	struct input_dev *input_device;
	
	printk("[GSLX680] Enter %s\n",__func__);
	ts->obj_data = &devices[ts->device_id];
	
	if(ts->device_id == 0)
	{
		ts->obj_data->data_size = MAX_FINGERS * ts->obj_data->touch_bytes + ts->obj_data->touch_meta_data;
		ts->obj_data->touch_index = 0;
	}
	
	ts->touch_data = kzalloc(ts->obj_data->data_size,GFP_KERNEL);
	if(!ts->touch_data)
	{
		printk("%s: Unable to allocate memory.\n",__func__);
		return -ENOMEM;
	}
	
	input_device = input_allocate_device();
	if(!input_device)
	{
		kfree(ts->touch_data);
		ret = -ENOMEM;
		return ret;
	}

	ts->input = input_device;
	
	input_device->name = GSLX680_I2C_NAME;
	input_device->id.bustype = BUS_I2C;
	input_device->dev.parent = &client->dev;
	input_set_drvdata(input_device,ts);
	
	set_bit(ABS_MT_POSITION_X, input_device->absbit);
	set_bit(ABS_MT_POSITION_Y, input_device->absbit);
	set_bit(ABS_MT_TOUCH_MAJOR, input_device->absbit);
	set_bit(ABS_MT_WIDTH_MAJOR, input_device->absbit);
	set_bit(ABS_PRESSURE, input_device->absbit);
	set_bit(BTN_TOUCH, input_device->keybit);
	set_bit(EV_ABS, input_device->evbit);
	set_bit(EV_KEY, input_device->evbit);
	set_bit(EV_SYN,input_device->evbit);
	__set_bit(INPUT_PROP_DIRECT, input_device->propbit);
	input_mt_init_slots(input_device, (MAX_CONTACTS + 1));
	 
	input_set_abs_params(input_device,ABS_MT_POSITION_X, 0, SCREEN_MAX_X, 0, 0);
	input_set_abs_params(input_device,ABS_MT_POSITION_Y, 0, SCREEN_MAX_Y, 0, 0);
	input_set_abs_params(input_device,ABS_MT_TOUCH_MAJOR, 0, PRESS_MAX, 0, 0);
	input_set_abs_params(input_device,ABS_MT_WIDTH_MAJOR, 0, 200, 0, 0);
       	input_set_abs_params(input_device,ABS_MT_TRACKING_ID, 0, 5, 0, 0);
	input_set_abs_params(input_device,ABS_PRESSURE, 0, PRESS_MAX, 0 , 0);
	input_set_abs_params(input_device,ABS_MT_PRESSURE, 0, PRESS_MAX, 0, 0);

	client->irq = IRQ_PORT;
	ts->irq = client->irq;

	ts->wq = create_singlethread_workqueue("workqueue_ts");
	if(!ts->wq)
	{
		printk("Could not create workqueue.\n");
		input_free_device(input_device);
		return -1;
	}

	INIT_WORK(&ts->work,gslx680_ts_workfunc);
	flush_workqueue(ts->wq);

	ret = input_register_device(input_device);
	if(ret)
		destroy_workqueue(ts->wq);
	return ret;
}

static void check_mem_data(struct i2c_client *client)
{
	u8 buf[4] = {0};

	msleep(30);
	read_chip_reg(client,0xb0,buf,sizeof(buf));

	if(buf[3] != 0x5a || buf[2] != 0x5a || buf[1] != 0x5a || buf[0] != 0x5a)
	{
		printk("#### check mem 0xb0  fail! The data is %x %x %x %x ####\n",buf[3],buf[2],buf[1],buf[0]);	
		gslx680_chip_init(client);
	}
}


static irqreturn_t gslx680_irq_handeler(int irq,void *dev_id)
{
	struct gsl_ts *ts = dev_id;
	printk("==============gslx680 interrupt===========\n");
	
	//shutdown irq and return now
	disable_irq_nosync(ts->irq);

	//ts->work is working?
	if(!work_pending(&ts->work))
		queue_work(ts->wq,&ts->work);
	return IRQ_HANDLE;	
}

static int gslx680_ts_probe(struct i2c_client *client,const struct i2c_device_id *id)
{
	int ret;
	struct gsl_ts *ts;

	printk("------------gslx680 ts probe-----------\n");
	if(!i2c_check_functionality(client->adapter,I2C_FUNC_I2C))
	{
		dev_err(&client->dev,"I2C functionality not supported\n");
		return -ENODEV;
	}
	
	ts = kzalloc(sizeof(*ts),GFP_KERNEL); 
	if(!ts)
		return -ENODEV;
	
	ts->client = client;
	i2c_set_clientdata(client,ts);
	ts->device_id = id->driver_data;
		
	if(source_init(client,ts)<0)
	{
		dev_err(&client->dev,"gslx680 init failed\n");
		input_free_device(ts->input);	
		kfree(ts);	
		return -1;
	}
	
	board_gpio_init();

	gslx680_chip_init(client);
	
	check_mem_data(client);
if(request_irq(client->irq,gslx680_irq_handeler,IRQ_TRIGGER_RISING,client->name,gslx680) < 0)
	{
		printk("gsl_probe request irq failed\n");
		free_irq(ts->irq,ts);
	}
	printk("probe end\n");	

	return 0;
}

static int gslx680_ts_remove(struct i2c_client *client)
{
	struct gsl_ts *ts = i2c_get_clientdata(client);
	printk("gslx680_ts_remove \n");
	
	//power maneger 
	device_init_wakeup(&client->dev,0);

	cancel_work_sync(&ts->work);
	free_irq(ts->irq,ts);
	destroy_workqueue(ts->wq);
	input_unregister_dev(ts->input);
	kfree(ts->touch_data);
	kfree(ts);
	return 0;	
}




static const struct i2c_device_id gslx680_ts_id[] = {
	{I2C_TP_NAME,0},
	{}	
} 

static struct i2c_driver gslx680_ts_driver ={
	.driver ={
		.name = I2C_TP_NAME,
		.owner = THIS_MODULE,
	}
	.probe = gslx680_ts_probe,
	.remove = gslx680_ts_remove,
	.id_table = gslx680_ts_id,	
}


static int __init gslx680_ts_init(void)
{
	int ret;
	printk("gslx680_ts_init\n");
	ret = i2c_add_driver(&gslx680_ts_driver);
	printk("register i2c for gslx680_ts ret = %d\n ",ret);
	return ret;
}

static void __exit gslx680_ts_exit(void)
{
	printk("gslx680_ts_exit\n");
	i2c_del_driver(&gslx680_ts_driver);
}



module_init(gslx680_ts_init);
module_exit(gslx680_ts_exit);


MODULE_LICENSE("GPL");









