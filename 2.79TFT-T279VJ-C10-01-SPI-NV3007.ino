/********************金逸晨**************************
*****************279VJ-C10-01 TFT   模块 2.79寸  8PIN TFT TFT FOR Arduino*************
*****3----3SCK,   4----4SDA,      5---5RST,
      6---6DC,      7----7BLK
****NV3007 控制器，本模块背光高电平开启，低电平关闭

从左往右，从上往下    240*320  16bit 5 6 5 RGB 模式  高位模式
********************************************************/
#define TFT_SCK_0  digitalWrite(3,LOW)             
#define TFT_SCK_1  digitalWrite(3,HIGH) 
#define TFT_SDA_0  digitalWrite(4,LOW)               
#define TFT_SDA_1  digitalWrite(4,HIGH)
#define TFT_RST_0  digitalWrite(5,LOW)               
#define TFT_RST_1  digitalWrite(5,HIGH)
#define TFT_DC_0  digitalWrite(6,LOW)               
#define TFT_DC_1  digitalWrite(6,HIGH)
#define TFT_CS_0  digitalWrite(7,LOW)               
#define  TFT_CS_1 digitalWrite(7,HIGH)
#define TFT_BL_0  digitalWrite(8,LOW)               
#define  TFT_BL_1 digitalWrite(8,HIGH)
void  IO_init(void )
{
pinMode(3,OUTPUT);//设置数字脚为输出
pinMode(4,OUTPUT);//设置数字脚为输出
pinMode(5,OUTPUT);//设置数字脚为输出
pinMode(6,OUTPUT);//设置数字脚为输出
pinMode(7,OUTPUT);//设置数字脚为输出
pinMode(8,OUTPUT);//设置数字脚为输出
}

#define DIS_DIR  0
#define     RED          0XF800	  //红色
#define     GREEN        0X07E0	  //绿色
#define     BLUE         0X001F	  //蓝色
#define     WHITE        0XFFFF	  //白色
#define     BLACK        0X0000  //黑色


#define TFT_COLUMN_NUMBER 142
#define TFT_LINE_NUMBER 428
#define TFT_COLUMN_OFFSET 12

#define PIC_NUM 28800     //图片数据大小

const unsigned char  *point;
const unsigned char chines_word[][32]=       //
{
{0x00,0x00,0xE4,0x3F,0x28,0x20,0x28,0x25,0x81,0x08,0x42,0x10,0x02,0x02,0x08,0x02,
0xE8,0x3F,0x04,0x02,0x07,0x07,0x84,0x0A,0x44,0x12,0x34,0x62,0x04,0x02,0x00,0x02},/*"深",0*/
{0x88,0x20,0x88,0x24,0x88,0x24,0x88,0x24,0x88,0x24,0xBF,0x24,0x88,0x24,0x88,0x24,
0x88,0x24,0x88,0x24,0x88,0x24,0xB8,0x24,0x87,0x24,0x42,0x24,0x40,0x20,0x20,0x20},/*"圳",1*/
{0x80,0x00,0x80,0x00,0x40,0x01,0x20,0x02,0x10,0x04,0x08,0x08,0xF4,0x17,0x83,0x60,
0x80,0x00,0xFC,0x1F,0x80,0x00,0x88,0x08,0x90,0x08,0x90,0x04,0xFF,0x7F,0x00,0x00},/*"金",2*/
{0x80,0x00,0x82,0x00,0x84,0x0F,0x44,0x08,0x20,0x04,0xF0,0x3F,0x27,0x22,0x24,0x22,
0xE4,0x3F,0x04,0x05,0x84,0x0C,0x84,0x54,0x44,0x44,0x24,0x78,0x0A,0x00,0xF1,0x7F},/*"逸",3*/
{0xF8,0x0F,0x08,0x08,0xF8,0x0F,0x08,0x08,0xF8,0x0F,0x00,0x00,0xFC,0x3F,0x04,0x00,
0xF4,0x1F,0x04,0x00,0xFC,0x7F,0x94,0x10,0x14,0x09,0x12,0x06,0x52,0x18,0x31,0x60},/*"晨",4*/
{0x80,0x00,0x80,0x00,0x80,0x00,0xFC,0x1F,0x84,0x10,0x84,0x10,0x84,0x10,0xFC,0x1F,
0x84,0x10,0x84,0x10,0x84,0x10,0xFC,0x1F,0x84,0x50,0x80,0x40,0x80,0x40,0x00,0x7F},/*"电",5*/
{0x00,0x00,0xFE,0x1F,0x00,0x08,0x00,0x04,0x00,0x02,0x80,0x01,0x80,0x00,0xFF,0x7F,
0x80,0x00,0x80,0x00,0x80,0x00,0x80,0x00,0x80,0x00,0x80,0x00,0xA0,0x00,0x40,0x00},/*"子",6*/
{0x00,0x00,0xE4,0x1F,0x48,0x10,0x48,0x10,0x41,0x10,0x82,0x08,0x92,0x08,0x90,0x08,
0x08,0x05,0x08,0x05,0x07,0x02,0x04,0x02,0x04,0x05,0x84,0x08,0x44,0x10,0x30,0x60},/*"汉",7*/
{0x40,0x00,0x80,0x00,0xFE,0x7F,0x02,0x40,0x01,0x20,0xF8,0x07,0x00,0x02,0x00,0x01,
0x80,0x00,0xFF,0x7F,0x80,0x00,0x80,0x00,0x80,0x00,0x80,0x00,0xA0,0x00,0x40,0x00},/*"字",8*/
{0x00,0x01,0x04,0x02,0xE8,0x3F,0x28,0x20,0x01,0x00,0xC2,0x1F,0x02,0x02,0xC8,0x1F,
0x48,0x12,0xC4,0x1F,0x47,0x12,0xC4,0x1F,0x04,0x00,0x84,0x08,0x44,0x10,0x20,0x20},/*"演",9*/
{0x00,0x00,0xFC,0x1F,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFF,0x7F,0x80,0x00,
0x80,0x00,0x88,0x08,0x88,0x10,0x84,0x20,0x82,0x40,0x81,0x40,0xA0,0x00,0x40,0x00},/*"示",10*/


};


void delay_us(unsigned int _us_time)
{       
  unsigned char x=0;
  for(;_us_time>0;_us_time--)
  {
    x++;
  }
}

/*************SPI配置函数*******************
SCL空闲时低电平，第一个上升沿采样
模拟SPI
******************************************/

/**************************SPI模块发送函数************************************************

 *************************************************************************/
void SPI_SendByte(unsigned  char byte)				//向液晶屏写一个8位数据
{
  
  unsigned char counter;
  for(counter=0;counter<8;counter++)
  { 
    TFT_SCK_0;  
    if((byte&0x80)==0)
    {
      TFT_SDA_0;
    }
    else TFT_SDA_1;
    byte=byte<<1;	
    TFT_SCK_1;			
  }
	TFT_SCK_0;
}

void TFT_SEND_CMD(unsigned char o_command)
  {
    TFT_CS_0;
	  TFT_DC_0;
    
    SPI_SendByte(o_command);
    TFT_CS_1;
   
    //TFT_DC_1;
  }
  //向液晶屏写一个8位数据
 //向液晶屏写一个8位数据
void TFT_SEND_DATA(unsigned char  o_data)
  { 
	  TFT_CS_0;
    TFT_DC_1;
    
    SPI_SendByte(o_data);
    TFT_CS_1;
    
   }
void TFT_SET_ADD(unsigned short int X,unsigned short int Y,unsigned short int X_END,unsigned short int Y_END)
{	
	X += TFT_COLUMN_OFFSET;
	X_END += TFT_COLUMN_OFFSET;
//            //判断地址是否合法
//    if((X_END>(TFT_COLUMN_NUMBER-1))|| (X>(TFT_COLUMN_NUMBER-1)) )      //横向向显示    //超出x地址范围
//    {
//        X_END = (TFT_COLUMN_NUMBER-1);
//            //X = 0;
//    }   
//    if ((Y_END > (TFT_LINE_NUMBER-1) )||(Y > (TFT_LINE_NUMBER-1) ))       //超出Y范围
//    {
//            //Y = 0;                          //移动到第一行
//            Y_END =  ( TFT_LINE_NUMBER-1);
//                
//    }
	TFT_SEND_CMD(0x2A);     //窗口地址横向                   
    TFT_SEND_DATA(X>>8);     
    TFT_SEND_DATA(X);//start column
	TFT_SEND_DATA(X_END>>8);     
    TFT_SEND_DATA(X_END);//start column
	TFT_SEND_CMD(0x2B);     //窗口地址纵向                   
    TFT_SEND_DATA(Y>>8);     
    TFT_SEND_DATA(Y);		//start row
    TFT_SEND_DATA(Y_END>>8);     
    TFT_SEND_DATA(Y_END);		//start row
    TFT_SEND_CMD(0x2C);     //写入数据   
}  
void TFT_Clear(unsigned short int back_c)									//清除液晶数据，刷黑
  {
    unsigned int ROW=0,column=0;
	TFT_SET_ADD(0x0000,0x0000,TFT_COLUMN_NUMBER-1,TFT_LINE_NUMBER-1);//纵向
    for(ROW=0;ROW<TFT_LINE_NUMBER;ROW++)             //ROW loop
      { 
          for(column=0;column<TFT_COLUMN_NUMBER;column++)  //column loop
            {         
				TFT_SEND_DATA(back_c>>8);
				TFT_SEND_DATA(back_c);
            }
      }
  }

void TFT_Init(void)        
  {
	TFT_RST_0;
	delay(100);
	TFT_RST_1;
	delay(120);
	//NV3006A1N IVO2.6 
	TFT_SEND_CMD(0xff);
	TFT_SEND_DATA(0xa5);  
	TFT_SEND_CMD(0x9a);
	TFT_SEND_DATA(0x08);
	TFT_SEND_CMD(0x9b);
	TFT_SEND_DATA(0x08);  
	TFT_SEND_CMD(0x9c);
	TFT_SEND_DATA(0xb0);  
	TFT_SEND_CMD(0x9d);
	TFT_SEND_DATA(0x16);
	TFT_SEND_CMD(0x9e);
	TFT_SEND_DATA(0xc4);
	TFT_SEND_CMD(0x8f);
	TFT_SEND_DATA(0x55);
	TFT_SEND_DATA(0x04);
	TFT_SEND_CMD(0x84);
	TFT_SEND_DATA(0x90);
	TFT_SEND_CMD(0x83);
	TFT_SEND_DATA(0x7b);
	TFT_SEND_CMD(0x85);
	TFT_SEND_DATA(0x33);
	TFT_SEND_CMD(0x60);
	TFT_SEND_DATA(0x00);
	TFT_SEND_CMD(0x70);
	TFT_SEND_DATA(0x00);
	TFT_SEND_CMD(0x61);
	TFT_SEND_DATA(0x02);
	TFT_SEND_CMD(0x71);
	TFT_SEND_DATA(0x02);
	TFT_SEND_CMD(0x62);
	TFT_SEND_DATA(0x04);
	TFT_SEND_CMD(0x72);
	TFT_SEND_DATA(0x04);
	TFT_SEND_CMD(0x6c);
	TFT_SEND_DATA(0x29);
	TFT_SEND_CMD(0x7c);
	TFT_SEND_DATA(0x29);
	TFT_SEND_CMD(0x6d);
	TFT_SEND_DATA(0x31);
	TFT_SEND_CMD(0x7d);
	TFT_SEND_DATA(0x31);
	TFT_SEND_CMD(0x6e);
	TFT_SEND_DATA(0x0f);
	TFT_SEND_CMD(0x7e);
	TFT_SEND_DATA(0x0f);
	TFT_SEND_CMD(0x66);
	TFT_SEND_DATA(0x21);
	TFT_SEND_CMD(0x76);
	TFT_SEND_DATA(0x21);
	TFT_SEND_CMD(0x68);
	TFT_SEND_DATA(0x3A);
	TFT_SEND_CMD(0x78);
	TFT_SEND_DATA(0x3A);
	TFT_SEND_CMD(0x63);
	TFT_SEND_DATA(0x07);
	TFT_SEND_CMD(0x73);
	TFT_SEND_DATA(0x07);
	TFT_SEND_CMD(0x64);
	TFT_SEND_DATA(0x05);
	TFT_SEND_CMD(0x74);
	TFT_SEND_DATA(0x05);
	TFT_SEND_CMD(0x65);
	TFT_SEND_DATA(0x02);
	TFT_SEND_CMD(0x75);
	TFT_SEND_DATA(0x02);
	TFT_SEND_CMD(0x67);
	TFT_SEND_DATA(0x23);
	TFT_SEND_CMD(0x77);
	TFT_SEND_DATA(0x23);
	TFT_SEND_CMD(0x69);
	TFT_SEND_DATA(0x08);
	TFT_SEND_CMD(0x79);
	TFT_SEND_DATA(0x08);
	TFT_SEND_CMD(0x6a);
	TFT_SEND_DATA(0x13);
	TFT_SEND_CMD(0x7a);
	TFT_SEND_DATA(0x13);
	TFT_SEND_CMD(0x6b);
	TFT_SEND_DATA(0x13);
	TFT_SEND_CMD(0x7b);
	TFT_SEND_DATA(0x13);
	TFT_SEND_CMD(0x6f);
	TFT_SEND_DATA(0x00);
	TFT_SEND_CMD(0x7f);
	TFT_SEND_DATA(0x00);
	TFT_SEND_CMD(0x50);
	TFT_SEND_DATA(0x00);
	TFT_SEND_CMD(0x52);
	TFT_SEND_DATA(0xd6);
	TFT_SEND_CMD(0x53);
	TFT_SEND_DATA(0x08);
	TFT_SEND_CMD(0x54);
	TFT_SEND_DATA(0x08);
	TFT_SEND_CMD(0x55);
	TFT_SEND_DATA(0x1e);
	TFT_SEND_CMD(0x56);
	TFT_SEND_DATA(0x1c);
	//goa map_sel
	TFT_SEND_CMD(0xa0);
	TFT_SEND_DATA(0x2b);
	TFT_SEND_DATA(0x24);
	TFT_SEND_DATA(0x00);
	TFT_SEND_CMD(0xa1);
	TFT_SEND_DATA(0x87);
	TFT_SEND_CMD(0xa2);
	TFT_SEND_DATA(0x86);
	TFT_SEND_CMD(0xa5);
	TFT_SEND_DATA(0x00);
	TFT_SEND_CMD(0xa6);
	TFT_SEND_DATA(0x00);
	TFT_SEND_CMD(0xa7);
	TFT_SEND_DATA(0x00);
	TFT_SEND_CMD(0xa8);
	TFT_SEND_DATA(0x36);
	TFT_SEND_CMD(0xa9);
	TFT_SEND_DATA(0x7e);
	TFT_SEND_CMD(0xaa);
	TFT_SEND_DATA(0x7e);
	TFT_SEND_CMD(0xB9);
	TFT_SEND_DATA(0x85);
	TFT_SEND_CMD(0xBA);
	TFT_SEND_DATA(0x84);
	TFT_SEND_CMD(0xBB);
	TFT_SEND_DATA(0x83);
	TFT_SEND_CMD(0xBC);
	TFT_SEND_DATA(0x82);
	TFT_SEND_CMD(0xBD);
	TFT_SEND_DATA(0x81);
	TFT_SEND_CMD(0xBE);
	TFT_SEND_DATA(0x80);
	TFT_SEND_CMD(0xBF);
	TFT_SEND_DATA(0x01);
	TFT_SEND_CMD(0xC0);
	TFT_SEND_DATA(0x02);
	TFT_SEND_CMD(0xc1);
	TFT_SEND_DATA(0x00);
	TFT_SEND_CMD(0xc2);
	TFT_SEND_DATA(0x00);
	TFT_SEND_CMD(0xc3);
	TFT_SEND_DATA(0x00);
	TFT_SEND_CMD(0xc4);
	TFT_SEND_DATA(0x33);
	TFT_SEND_CMD(0xc5);
	TFT_SEND_DATA(0x7e);
	TFT_SEND_CMD(0xc6);
	TFT_SEND_DATA(0x7e);
	TFT_SEND_CMD(0xC8);
	TFT_SEND_DATA(0x33);
	TFT_SEND_DATA(0x33);
	TFT_SEND_CMD(0xC9);
	TFT_SEND_DATA(0x68);
	TFT_SEND_CMD(0xCA);
	TFT_SEND_DATA(0x69);
	TFT_SEND_CMD(0xCB);
	TFT_SEND_DATA(0x6a);
	TFT_SEND_CMD(0xCC);
	TFT_SEND_DATA(0x6b);
	TFT_SEND_CMD(0xCD);
	TFT_SEND_DATA(0x33);
	TFT_SEND_DATA(0x33); 
	TFT_SEND_CMD(0xCE);
	TFT_SEND_DATA(0x6c);
	TFT_SEND_CMD(0xCF);
	TFT_SEND_DATA(0x6d);
	TFT_SEND_CMD(0xD0);
	TFT_SEND_DATA(0x6e);
	TFT_SEND_CMD(0xD1);
	TFT_SEND_DATA(0x6f);
	TFT_SEND_CMD(0xAB);
	TFT_SEND_DATA(0x03);
	TFT_SEND_DATA(0x67);
	TFT_SEND_CMD(0xAC);
	TFT_SEND_DATA(0x03);
	TFT_SEND_DATA(0x6b);
	TFT_SEND_CMD(0xAD);
	TFT_SEND_DATA(0x03);
	TFT_SEND_DATA(0x68);
	TFT_SEND_CMD(0xAE);
	TFT_SEND_DATA(0x03);
	TFT_SEND_DATA(0x6c);
	TFT_SEND_CMD(0xb3);
	TFT_SEND_DATA(0x00);
	TFT_SEND_CMD(0xb4);
	TFT_SEND_DATA(0x00);
	TFT_SEND_CMD(0xb5);
	TFT_SEND_DATA(0x00);
	TFT_SEND_CMD(0xB6);
	TFT_SEND_DATA(0x32);
	TFT_SEND_CMD(0xB7);
	TFT_SEND_DATA(0x7e);
	TFT_SEND_CMD(0xB8);
	TFT_SEND_DATA(0x7e);
	TFT_SEND_CMD(0xe0);
	TFT_SEND_DATA(0x00);
	TFT_SEND_CMD(0xe1);
	TFT_SEND_DATA(0x03);
	TFT_SEND_DATA(0x0f);
	TFT_SEND_CMD(0xe2);
	TFT_SEND_DATA(0x04);
	TFT_SEND_CMD(0xe3);
	TFT_SEND_DATA(0x01);
	TFT_SEND_CMD(0xe4);
	TFT_SEND_DATA(0x0e);
	TFT_SEND_CMD(0xe5);
	TFT_SEND_DATA(0x01);
	TFT_SEND_CMD(0xe6);
	TFT_SEND_DATA(0x19);
	TFT_SEND_CMD(0xe7);
	TFT_SEND_DATA(0x10);
	TFT_SEND_CMD(0xe8);
	TFT_SEND_DATA(0x10);
	TFT_SEND_CMD(0xea);
	TFT_SEND_DATA(0x12);
	TFT_SEND_CMD(0xeb);
	TFT_SEND_DATA(0xd0);
	TFT_SEND_CMD(0xec);
	TFT_SEND_DATA(0x04);
	TFT_SEND_CMD(0xed);
	TFT_SEND_DATA(0x07);
	TFT_SEND_CMD(0xee);
	TFT_SEND_DATA(0x07);
	TFT_SEND_CMD(0xef);
	TFT_SEND_DATA(0x09);
	TFT_SEND_CMD(0xf0);
	TFT_SEND_DATA(0xd0);
	TFT_SEND_CMD(0xf1);
	TFT_SEND_DATA(0x0e);
	 
	//TFT_SEND_CMD(0xF9);
	//TFT_SEND_DATA(0x17); 
	//TFT_SEND_CMD(0xf2);
	//TFT_SEND_DATA(0x2e);
	//TFT_SEND_DATA(0x1b);
	//TFT_SEND_DATA(0x0b);
	//TFT_SEND_DATA(0x20);
	//TFT_SEND_CMD(0xF9);
	TFT_SEND_DATA(0x17); 
	TFT_SEND_CMD(0xf2);
	TFT_SEND_DATA(0x2c);
	TFT_SEND_DATA(0x1b);
	TFT_SEND_DATA(0x0b);
	TFT_SEND_DATA(0x20);
	////1 dot
	TFT_SEND_CMD(0xe9);
	TFT_SEND_DATA(0x29);
	TFT_SEND_CMD(0xec);
	TFT_SEND_DATA(0x04);
	//TE
	TFT_SEND_CMD(0x35);
	TFT_SEND_DATA(0x00); 
	TFT_SEND_CMD(0x44);
	TFT_SEND_DATA(0x00);
	TFT_SEND_DATA(0x10); 
	TFT_SEND_CMD(0x46);
	TFT_SEND_DATA(0x10); 
	TFT_SEND_CMD(0xff);
	TFT_SEND_DATA(0x00); 
	TFT_SEND_CMD(0x3a);
	TFT_SEND_DATA(0x05); 
	TFT_SEND_CMD(0x11); 
	delay(220); 
	TFT_SEND_CMD(0x29); 
//	delay(200);
  }
void display_char16_16(unsigned int x,unsigned int y,unsigned long color,unsigned char word_serial_number)
{
   unsigned int column;
  unsigned char tm=0,temp=0,xxx=0;   
   TFT_SET_ADD(x,y,x+15,y+15);//纵向 
  for(column=0;column<32;column++)  //column loop
          {
        temp=chines_word[  word_serial_number ][xxx];
        for(tm=0;tm<8;tm++)
        {
        if(temp&0x01)
          {
          TFT_SEND_DATA(color>>8);
          TFT_SEND_DATA(color);
          }
        else 
          {
          TFT_SEND_DATA(0XFF);
          TFT_SEND_DATA(0XFF);
          }
          temp>>=1;
        }
        xxx++;
          
      }
}

void Picture_Display(const unsigned char *ptr_pic)
{
    unsigned long  number;
	TFT_SEND_CMD(0x2a); 		//Column address set
	TFT_SEND_DATA(0x00); 		//start column
	TFT_SEND_DATA(0x00); 
	TFT_SEND_DATA(0x00);		//end column
	TFT_SEND_DATA(0x77);

	TFT_SEND_CMD(0x2b); 		//Row address set
	TFT_SEND_DATA(0x00); 		//start row
	TFT_SEND_DATA(0x00); 
	TFT_SEND_DATA(0x00);		//end row
	TFT_SEND_DATA(0x78);
	TFT_SEND_CMD(0x2C);			//Memory write
	TFT_SET_ADD(5,5,124,124);  
	for(number=0;number<PIC_NUM;number++)	
          {
//            data=*ptr_pic++;
//            data=~data;
            TFT_SEND_DATA(*ptr_pic++);
	
			
          }
  }
void setup() {

	IO_init();
	TFT_BL_1;
	TFT_Init();
}
void loop()
{ 
	TFT_BL_0;
	TFT_Init();
	TFT_BL_1;
	while(1)
	{	
	TFT_Clear(RED);
	delay(2000);
	TFT_Clear(GREEN);
	delay(2000);
	TFT_Clear(BLUE);
	delay(3000);
	TFT_Clear(BLACK);
	Picture_Display(point);
	delay(3000);
    display_char16_16(0,160,BLUE,0);
	display_char16_16(20,160,GREEN,1);
	display_char16_16(40,160,RED,2);
	display_char16_16(60,160,BLUE,3);
	display_char16_16(80,160,GREEN,4);
	display_char16_16(100,160,BLUE,5);
	display_char16_16(120,160,RED,6);
	delay(10000);

	}
}








