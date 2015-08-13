/*
struct v4l2_send_command_control {
	__u32		     id;
	__u32		     value0;
	__u32		     value1;
	char	     	 debug[256];
};
*/

#define VIDIOC_SEND_COMMAND		_IOWR('V', 92, struct v4l2_send_command_control)

