#include <linux/module.h>
#include <linux/init.h>
#include <linux/moduleparam.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/types.h>
#include <linux/cdev.h>
#include <linux/highmem.h>
#include <linux/mm.h>
#include <linux/io.h>
#include <linux/list.h>

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Page replacement policy");
MODULE_AUTHOR("CCU CSIE");

// file operations
static int test_open(struct inode *inode, struct file *fp);
static int test_release(struct inode *inode, struct file *fp);
static long test_ioctl(struct file *fp, unsigned int cmd, unsigned long virtual_addr);
static int __init test_init(void);
static void __exit test_exit(void);

// Accounting information
// 可以自行紀錄所有轉換後的 request 與原始 request 對應關係
// 或者其他資訊如 page fault 次數, replace 次數等等
#define FRAME_NUMBER (256)
#define REQ_NUMBER   (4096)
#define FIFO (0)
#define LRU  (1)
static int currReqNo = 0;  //代表現在是第幾筆 user request
static int pagefault = 0;
static int replace = 0;
static int currentVictimNo = 0;  //代表現在的 victim frame 是多少
static int replace_policy = FIFO;

// Data request information
// 使用者 input request 資訊, 含有轉換關係, 是否在 disk 中, 是否屬於 update
struct request{
	int virtual_addr;
	int physical_addr;
	int alreadyInDisk;
	int update;
};
// reqMeta 是 input request 的歷史紀錄
// 越後面的代表越新的紀錄
static struct request reqMeta[REQ_NUMBER];

// Free list
// 表示當下 256 個 memory frame 的狀態, 用來給系統管理可用 memory
// 可自行修改增加, 只要結果正確即可
struct free_list{
	struct list_head next;
	int virtual_addr;  //紀錄原始 request 的 virtual address
	int ref_count;     //紀錄曾經有多少 request 曾經 access 過這個 frame
	int free_bit;      //當下這個 frame 是否為空可以直接寫入
};
static struct free_list *free_list_header;

// Function declartions
static void add_free_list_tail(void);
static void delete_all_free_list(void);
static void delete_free_list(int virtual_addr);
static void print_information(void);
static int paging_victim_selection(void);
static int paging_find_free_page(struct request user_req);

// Module structures
static struct file_operations my_fops = {
	.owner = THIS_MODULE,
	.open = test_open,
	.release = test_release,
	.unlocked_ioctl = test_ioctl,
};

struct cdev myCdev = {
	.owner = THIS_MODULE,
	.ops = &my_fops,
};
static dev_t devNo;
static unsigned int myModule_nr_devs = 1;

// IOCTL commands
#define MY_IOCTL_QUIRK 'c'
#define CMD_WRITE_REQUEST  _IOW(MY_IOCTL_QUIRK, 1, char)
#define CMD_PRINT_INFO     _IOR(MY_IOCTL_QUIRK, 2, char)
static int cmd_choice = 0;

static long test_ioctl(struct file *fp, unsigned int cmd, unsigned long virtual_addr)
{
	cmd_choice = cmd;

	switch(cmd_choice){
		case CMD_WRITE_REQUEST:
			 
			//紀錄新近來 request 的 metadata
			//紀錄 virtual addr, physical addr, 是否已經在 disk內, 是否屬於 update
			//Modify here
			reqMeta[currReqNo].virtual_addr = virtual_addr;
			reqMeta[currReqNo].physical_addr = virtual_addr%256;
//			reqMeta[currReqNo].update = 0;
//			reqMeta[currReqNo].alreadyInDisk = 0;

			paging_find_free_page(reqMeta[currReqNo]);
			break;

		case CMD_PRINT_INFO:
			print_information();
			break;
	}

    currReqNo++;
	return 0;
}


static int test_open(struct inode *inode, struct file *fp)
{
	return 0;
}

static int test_release(struct inode *inode, struct file *fp)
{
	printk("Release ok\n");
	return 0;
}

static ssize_t test_write(struct file *fp, const char __user *buf, size_t size, loff_t *position)
{
	return 0;
}

static ssize_t test_read(struct file *fp, char __user *buf, size_t size, loff_t *position)
{
	return 0;
}

static void add_free_list_tail(void)
{
	struct free_list *tmp_node;
	tmp_node = kmalloc(sizeof(struct free_list), GFP_KERNEL);
	INIT_LIST_HEAD(&tmp_node->next);

	tmp_node->virtual_addr = -1;
	tmp_node->ref_count = 0;
	tmp_node->free_bit = 1;

	list_add_tail(&tmp_node->next, &free_list_header->next);
}


//回傳 victim physical frame 
static int paging_victim_selection(void)
{
    int victim_no;
    if(replace_policy == FIFO){
        //Modify here
		victim_no = reqMeta[currentVictimNo].physical_addr;
		if(currentVictimNo == 255){
			currentVictimNo = 0;
		}
		else{
			currentVictimNo += 1;
		}
    }
    else if(replace_policy == LRU){
		//Modify here
		struct free_list *tmp_node;
		int ref_count_min = 9999;
		victim_no = 0;
		
		list_for_each_entry(tmp_node, &free_list_header->next, next){
			if(tmp_node->ref_count < ref_count_min){
				ref_count_min = tmp_node->ref_count;
				victim_no = tmp_node->virtual_addr%256;
			}
		}
    }
    return victim_no;
}


static int paging_find_free_page(struct request user_req)
{
	struct free_list *tmp_node;
	int memAvailable = 0;
	int victim_no;
	unsigned long victim_virtual;
	unsigned long victim_physical;
	int i = 0;
	int found_in_memory = 0;
	
	//步驟一：
	//檢查 free_list 是否存在新進來的 user_req ?
	//對所有的 free_list node 一一檢查所存之 virtual addr 是否與 user_req 的 virtual_addr 相符
	//若有找到代表是 update, 只要對這個 free_list node 更新 ref_count 和對現在的 user_req 紀錄為是屬於 update
    //然後結束
	list_for_each_entry(tmp_node, &free_list_header->next, next){
		if(tmp_node->virtual_addr == user_req.virtual_addr){
			found_in_memory = 1;
		 
			printk("user_req:%d found in memory\n", user_req.virtual_addr);
			tmp_node->ref_count += 1;
			reqMeta[currReqNo].update = 1;	
			break;
		}
    }
	if(found_in_memory) return 1;



    //步驟二：
	//執行到這裡代表當前 memory 不存在這個 user_req
	//所以先找找看當前 memory 有沒有空間可以寫？
	//對所有 free_list 逐一檢查, 找出可用空間位置
   	list_for_each_entry(tmp_node, &free_list_header->next, next){
        if(tmp_node->free_bit == 1){
			memAvailable = 1;
			break;
        }
    }

    //步驟三之一：
	//有找到空間可以給新進來的 user_req
	//接著在這空位寫入 user_req
	//所以此空位改狀態為已寫入
	//存放 user_req 的 virtual_addr
	//更新 ref_count
   	if(memAvailable){
		tmp_node->free_bit = 0;
		tmp_node->virtual_addr = user_req.virtual_addr;
		tmp_node->ref_count += 1;
		return 1;
    }

    //步驟三之二：
	//當前沒有可用空間, 所以要先找 victim frame 並執行置換
   	else{
		//找 victim frame 之 free list node 的位置
		//並找出 victim frame 代表的 virtual address
        victim_physical = paging_victim_selection();
		int i=0;
		list_for_each_entry(tmp_node, &free_list_header->next, next){
           	//Modify here
	        if(tmp_node->virtual_addr%256 == victim_physical){
				break;
       		}
		}
        victim_virtual = tmp_node->virtual_addr;
    
        //倒回去檢查 victim 在 reqMeta 的紀錄（因為越後面越新的狀態), 找出 victim 最新的資訊
        for(i=currReqNo; i>=0; i--){
		   	//若 history 找到最新 victim frame 所存的 victim_virtual 資訊
	   		if(reqMeta[i].virtual_addr == victim_virtual){
				//更新 victim 在 free_list 的紀錄, 把 victim frame 內容覆蓋成 user_req
				//Modify here
				tmp_node->virtual_address = reqMeta[i].virtual_address;
				//case1: 檢查這個 victim 是不是已經 update 過, 若沒 update 過代表只須增加 replace 次數
		   		if(reqMeta[i].update == 0){
					replace++;
				}
				//case 2: victim 已經 update 過, 代表需要 writeback to disk, replace 次數與 page fault 槭樹增加
		   		//        NOTE: 這是嚴格的 page fault 定義, 若要寬鬆的定義直接看 page replacement 即可
		   		else if(reqMeta[i].update == 1){
					reqMeta[i].update = 0;
					reqMeta[i].alreadyInDisk = 1;
					replace++;
					pagefault++;
				}

				break;
      	 	}
    	}
	}//else
		
    return 0;
}


static void print_information(void)
{
	struct free_list *tmp_node;
	int i = 0;
	if(list_empty(&free_list_header->next)){
		return;
	}

	printk("==== Printing current memory status ====\n");
	list_for_each_entry(tmp_node, &free_list_header->next, next){
			printk("virtual_addr:%d, physical_addr:%d, PageRef:%d, free:%d\n", 
					tmp_node->virtual_addr,
					i,
					tmp_node->ref_count,
					tmp_node->free_bit);
			i++;
	}

	printk("==== Printing misc information ====");
	printk("Total replacements:%d, page faults:%d\n", replace, pagefault);

	/*
	printk("==== Printing user request history ====\n");
	for(i=0; i<REQ_NUMBER; i++)
		printk("User_req virtual_addr=%d, physical_addr = %d, disk=%d, update=%d\n",
				reqMeta[i].virtual_addr,
				reqMeta[i].physical_addr,
				reqMeta[i].alreadyInDisk,
				reqMeta[i].update
		);
	*/
}


// 刪除 free_list 某一個 virtual addr 對應的 frame
static void delete_free_list(int virtual_addr)
{
	struct free_list *cur_node, *tmp_node;
	if(list_empty(&free_list_header->next)){
		return;
	}

	list_for_each_entry_safe(cur_node, tmp_node, &free_list_header->next, next){
		if(cur_node->virtual_addr == virtual_addr){
			list_del_init(&cur_node->next);
			kfree(cur_node);
			cur_node = NULL;
			break;
		}
	}
}


// 銷毀整個 free list
static void delete_all_free_list(void)
{
	struct free_list *cur_node, *tmp_node;
	if(list_empty(&free_list_header->next)){
		return;
	}

	list_for_each_entry_safe(cur_node, tmp_node, &free_list_header->next, next){
		list_del_init(&cur_node->next);
		kfree(cur_node);
	}
}


static int __init test_init(void)
{
	int ret;
	int i;
	printk("Start to allocate kernel memory\n");

	//init free list
	free_list_header = kmalloc(sizeof(struct free_list), GFP_KERNEL);
	INIT_LIST_HEAD(&free_list_header->next);
	for(i=0; i<FRAME_NUMBER; i++)
		add_free_list_tail();

	//init request meta history
	for(i=0; i<REQ_NUMBER; i++){
	     reqMeta[i].virtual_addr = -1;
	     reqMeta[i].physical_addr = -1;
	     reqMeta[i].alreadyInDisk = -1;
	     reqMeta[i].update = -1;
	}

	//init character device
	ret = alloc_chrdev_region(&devNo, 30, 1, "myChrDevice");
	if(ret < 0)
    	    printk("Create chrdev failed.\n");
	else
    	    printk("/dev/myChrDevice regiester major %i, minor %i\n", MAJOR(devNo), MINOR(devNo) );

	cdev_init(&myCdev, &my_fops);
	ret = cdev_add(&myCdev, devNo, 1);
	if(ret <0)
		printk("Add chrdev failed.\n");
	return 0;
}

static void __exit test_exit(void)
{
	printk("Module exit...\n");
	delete_all_free_list();

	cdev_del(&myCdev);
	unregister_chrdev_region(devNo, myModule_nr_devs);
	printk("Unregister character device successfully.\n");
}

module_init(test_init);
module_exit(test_exit);
