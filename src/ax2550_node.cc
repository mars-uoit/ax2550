#include "ros/ros.h"
#include "geometry_msgs/Twist.h"
#include "nav_msgs/Odometry.h"
#include "ax2550/StampedEncoders.h"
#include "tf/tf.h"
#include <tf/transform_broadcaster.h>

#include <string>
#include <cmath>

#include "ax2550/ax2550.h"

using namespace ax2550;
using std::string;

AX2550 *mc;
ros::Publisher odom_pub;
ros::Publisher encoder_pub;
tf::TransformBroadcaster *odom_broadcaster;

static double ENCODER_RESOLUTION = 250*4;
double wheel_circumference = 31.0;
double wheel_base_length = 44.5;
double wheel_diameter = 10.0;
double encoder_poll_rate;
std::string odom_frame_id;
size_t error_count;
double target_speed = 0.0;
double target_direction = 0.0;

double rot_cov = 0.0;
double pos_cov = 0.0;

static double A_MAX = 50.0;
static double B_MAX = 50.0;

const int timeout_sec = 2.0;
ros::Time time_last;

// Persistent variables
double prev_x = 0, prev_y = 0, prev_w = 0;
ros::Time prev_time;

double wrapToPi(double angle) 
{
    angle += M_PI;
    bool is_neg = (angle < 0);
    angle = fmod(angle, (2.0*M_PI));
    if (is_neg) 
    {
        angle += (2.0*M_PI);
    }
    angle -= M_PI;
    return angle;
}

void cmd_velCallback(const geometry_msgs::Twist::ConstPtr& msg) 
{
    time_last = ros::Time::now();
    if(mc == NULL || !mc->isConnected())
        return;
    // Convert mps to rpm
    double A = msg->linear.x;
    double B = msg->angular.z * (wheel_base_length/2.0);
    
    double A_rpm = A * (60.0 / (M_PI*wheel_diameter));
    double B_rpm = B * (60.0 / (M_PI*wheel_diameter));
    
    // Convert rpm to relative
    double A_rel = (A_rpm * 250 * 64) / 58593.75;
    double B_rel = (B_rpm * 250 * 64) / 58593.75;
    
    // ROS_INFO("Arpm: %f, Arel: %f, Brpm: %f, Brel: %f", A_rpm, A_rel, B_rpm, B_rel);
    
    // Bounds check
    if(A_rel > A_MAX)
        A_rel = A_MAX;
    if(A_rel < -1*A_MAX)
        A_rel = -1*A_MAX;
    if(B_rel > B_MAX)
        B_rel = B_MAX;
    if(B_rel < -1*B_MAX)
        B_rel = -1*B_MAX;

    // Set the targets
    target_speed = A_rel;
    target_direction = B_rel;
}

void controlLoop() 
{
    // ROS_INFO("Relative move commands: %f %f", target_speed, target_direction);
    try 
    {
	//SAFETY TIME OUT
	//if a command is not received for 2 seconds the motors time out
	double time_past = (ros::Time::now() - time_last).toSec();
	if(time_past > timeout_sec)
	{
		mc->move(0,0);
		ROS_WARN("No velocity commands received - motors timed out");
	}
	else
	{
		mc->move(target_speed, target_direction);
	}
    } 
    catch(const std::exception &e) 
    {
    	if (string(e.what()).find("did not receive") != string::npos || string(e.what()).find("failed to receive an echo") != string::npos) 
	{
            ROS_WARN("Error commanding the motors: %s", e.what());
    	} 
	else 
	{
            ROS_ERROR("Error commanding the motors: %s", e.what());
            mc->disconnect();
    	}
    }
}

void errorMsgCallback(const std::string &msg) 
{
    ROS_ERROR("%s", msg.c_str());
}

void warnMsgCallback(const std::string &msg) 
{
    ROS_WARN("%s", msg.c_str());
}

void infoMsgCallback(const std::string &msg) 
{
    ROS_INFO("%s", msg.c_str());
}

void debugMsgCallback(const std::string &msg) 
{
    ROS_DEBUG("%s", msg.c_str());
}

void queryEncoders() 
{
    // Make sure we are connected
    if(!ros::ok() || mc == NULL || !mc->isConnected())
        return;
    
    long encoder1, encoder2;
    ros::Time now = ros::Time::now();
    // Retreive the data
    try 
    {
        mc->queryEncoders(encoder1, encoder2, true);
        if (error_count > 0) 
	{
            error_count -= 1;
        }
    } 
    catch(std::exception &e) 
    {
        if (string(e.what()).find("failed to receive ") != string::npos && error_count != 10) 
	{
            error_count += 1;
            ROS_WARN("Error reading the Encoders: %s", e.what());    
        } 
	else 
	{
            ROS_ERROR("Error reading the Encoders: %s", e.what());
            mc->disconnect();
        }
        return;
    }
    
    double delta_time = (now - prev_time).toSec();
    prev_time = now;
    
    // Convert to mps for each wheel from delta encoder ticks
    double left_v = encoder1 * 2*M_PI / ENCODER_RESOLUTION;
    left_v /= delta_time;
    // left_v *= encoder_poll_rate;
    double right_v = -encoder2 * 2*M_PI / ENCODER_RESOLUTION;
    right_v /= delta_time;
    // right_v *= encoder_poll_rate;
    
    ax2550::StampedEncoders encoder_msg;
    
    encoder_msg.header.stamp = now;
    encoder_msg.header.frame_id = "base_link";
    encoder_msg.encoders.time_delta = delta_time;
    encoder_msg.encoders.left_wheel = encoder1;
    encoder_msg.encoders.right_wheel = -encoder2;
    
    encoder_pub.publish(encoder_msg);

    double v = 0.0;
    double w = 0.0;
    
    double r_L = wheel_diameter/2.0;
    double r_R = wheel_diameter/2.0;
    
    v += r_L/2.0 * left_v;
    v += r_R/2.0 * right_v;

    w += r_R/wheel_base_length * right_v;
    w -= r_L/wheel_base_length * left_v;

    
    // Update the states based on model and input
    prev_x += delta_time * v * cos(prev_w + delta_time * (w/2.0));
    prev_y += delta_time * v * sin(prev_w + delta_time * (w/2.0));
    prev_w += delta_time * w;
    prev_w = wrapToPi(prev_w);
    
    // ROS_INFO("%f", prev_w);
    
    geometry_msgs::Quaternion quat = tf::createQuaternionMsgFromYaw(prev_w);
    
    // Populate the msg
    nav_msgs::Odometry odom_msg;
    odom_msg.header.stamp = now;
    odom_msg.header.frame_id = odom_frame_id;
    odom_msg.pose.pose.position.x = prev_x;
    odom_msg.pose.pose.position.y = prev_y;
    odom_msg.pose.pose.orientation = quat;
    odom_msg.pose.covariance[0] = pos_cov;
    odom_msg.pose.covariance[7] = pos_cov;
    odom_msg.pose.covariance[14] = 1e100;
    odom_msg.pose.covariance[21] = 1e100;
    odom_msg.pose.covariance[28] = 1e100;
    odom_msg.pose.covariance[35] = rot_cov;
    
    // odom_msg.twist.twist.linear.x = v/delta_time;
    odom_msg.twist.twist.linear.x = v;
    // odom_msg.twist.twist.angular.z = w/delta_time;
    odom_msg.twist.twist.angular.z = w;
    
    odom_pub.publish(odom_msg);
    
    // TODO: Add TF broadcaster
    // geometry_msgs::TransformStamped odom_trans;
    //     odom_trans.header.stamp = now;
    //     odom_trans.header.frame_id = "odom";
    //     odom_trans.child_frame_id = "base_footprint";
    // 
    //     odom_trans.transform.translation.x = prev_x;
    //     odom_trans.transform.translation.y = prev_y;
    //     odom_trans.transform.translation.z = 0.0;
    //     odom_trans.transform.rotation = quat;
    //     
    //     odom_broadcaster->sendTransform(odom_trans);
}

int main(int argc, char **argv) {
    // Node setup
    ros::init(argc, argv, "ax2550_node");
    ros::NodeHandle n;
    prev_time = ros::Time::now();
    
    // Serial port parameter
    std::string port;
    n.param("serial_port", port, std::string("/dev/serial/by-id/usb-FTDI_FT232R_USB_UART_A501IKWU-if00-port0")); ///dev/motor_controller
    
    // Wheel diameter parameter
    n.param("wheel_diameter", wheel_diameter, 0.3048);
    
    wheel_circumference = wheel_diameter * M_PI;
    
    // Wheel base length
    n.param("wheel_base_length", wheel_base_length, 0.9144);
    
    // Odom Frame id parameter
    n.param("odom_frame_id", odom_frame_id, std::string("odom"));

    // Load up some covariances from parameters
    n.param("rotation_covariance",rot_cov, 1.0);
    n.param("position_covariance",pos_cov, 1.0);
    
    // Setup Encoder polling
    n.param("encoder_poll_rate", encoder_poll_rate, 25.0);
    ros::Rate encoder_rate(encoder_poll_rate);
    
    // Odometry Publisher
    odom_pub = n.advertise<nav_msgs::Odometry>("odom", 5);
    
    // Encoder Publisher
    encoder_pub = n.advertise<ax2550::StampedEncoders>("encoders", 5);
    
    // TF Broadcaster
    odom_broadcaster = new tf::TransformBroadcaster;
    
    // cmd_vel Subscriber
    ros::Subscriber sub = n.subscribe("cmd_vel", 1, cmd_velCallback);
    
    // Spinner
    ros::AsyncSpinner spinner(1);
    spinner.start();

    while(ros::ok()) 
    {
        ROS_INFO("AX2550 connecting to port %s", port.c_str());
        try 
	{
            mc = new AX2550();
            mc->warn = warnMsgCallback;
            mc->info = infoMsgCallback;
            mc->debug = debugMsgCallback;
            mc->connect(port);
        } 
	catch(std::exception &e) 
	{
            ROS_ERROR("Failed to connect to the AX2550: %s", e.what());
            if (mc != NULL) 
	    {
            	mc->disconnect();
            }
        }
        int count = 0;
        while(mc != NULL && mc->isConnected() && ros::ok()) 
	{
            queryEncoders();
            if (count == 1) 
	    {
                controlLoop();
                count = 0;
            } 
	    else 
	    {
                count += 1;
            }
	    encoder_rate.sleep();
        }
        if (mc != NULL) 
	{
        	delete mc;
        }
        mc = NULL;
        if(!ros::ok())
            break;
        ROS_INFO("Will try to reconnect to the AX2550 in 5 seconds.");
        for (int i = 0; i < 100; ++i) 
	{
        	ros::Duration(5.0/100.0).sleep();
        	if (!ros::ok())
        	break;
        }
        target_speed = 0.0;
        target_direction = 0.0;
    }

    spinner.stop();
    
    return 0;
}
