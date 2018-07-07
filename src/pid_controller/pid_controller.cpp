#include <ros/ros.h>
#include <ros/package.h>

#include <dynamic_reconfigure/server.h>
#include <mrs_msgs/AttitudeCommand.h>
#include <nav_msgs/Odometry.h>
#include <tf/transform_datatypes.h>

#include <math.h>

#include <mrs_msgs/ControllerStatus.h>
#include <mrs_mav_manager/Controller.h>

#include <mrs_controllers/pid_gainsConfig.h>

using namespace std;

namespace mrs_controllers
{

//{ PID

class Pid {

public:
  Pid(std::string name, double kp, double kd, double ki, double integral_saturation, double saturation, double exp_filter_const);

  double update(double error, double dt);

  void reset(double last_error);

  void setGains(double kp, double kd, double ki);

private:
  double integral;
  double last_error;

  // gains
  double kp;
  double kd;
  double ki;
  double exp_filter_const;
  double integral_saturation;
  double saturation;

  std::string name;
};

void Pid::setGains(double kp, double kd, double ki) {

  this->kp = kp;
  this->kd = kd;
  this->ki = ki;
}

Pid::Pid(std::string name, double kp, double kd, double ki, double integral_saturation, double saturation, double exp_filter_const) {

  this->name = name;

  this->kp                  = kp;
  this->kd                  = kd;
  this->ki                  = ki;
  this->integral_saturation = integral_saturation;
  this->saturation          = saturation;
  this->exp_filter_const    = exp_filter_const;

  integral   = 0;
  last_error = 0;
}

double Pid::update(double error, double dt) {

  // calculate the filtered difference
  double difference = exp_filter_const * last_error + (1 - exp_filter_const) * ((error - last_error) / dt);
  last_error        = error;

  /* ROS_INFO("%s error=%f", name.c_str(), error); */

  // calculate the pid action
  double control_output = kp * error + kd * difference + ki * integral;

  /* ROS_INFO("%s output=%f", name.c_str(), control_output); */

  // saturate the control output
  bool saturated = false;
  if (!std::isfinite(control_output)) {
    control_output = 0;
    ROS_WARN_THROTTLE(1.0, "NaN detected in variable \"control_output\", setting it to 0 and returning!!!");
  } else if (control_output > saturation) {
    control_output = saturation;
    saturated      = true;
  } else if (control_output < -saturation) {
    control_output = -saturation;
    saturated      = true;
  }

  if (saturated) {
    ROS_WARN_THROTTLE(1.0, "The '%s' PID is being saturated!", name.c_str());

    // integrate only in the direction oposite to the saturation (antiwindup)
    if (control_output > 0 && error < 0) {
      integral += error;
    } else if (control_output < 0 && error > 0) {
      integral += error;
    }
  } else {
    // if the output is not saturated, we do not care in which direction do we integrate
    integral += error;
  }

  // saturate the integral
  saturated = false;
  if (!std::isfinite(integral)) {
    integral = 0;
    ROS_WARN_THROTTLE(1.0, "NaN detected in variable \"integral\", setting it to 0 and returning!!!");
  } else if (integral > integral_saturation) {
    integral  = integral_saturation;
    saturated = true;
  } else if (integral < -integral_saturation) {
    integral  = -integral_saturation;
    saturated = true;
  }

  if (saturated) {
    ROS_WARN_THROTTLE(1.0, "The '%s' PID's integral is being saturated!", name.c_str());
  }

  return control_output;
}

void Pid::reset(double last_error) {

  integral         = 0;
  this->last_error = last_error;
}

//}

class PidController : public mrs_mav_manager::Controller {

public:
  PidController(void);

  void Initialize(const ros::NodeHandle &parent_nh);
  bool Activate(void);
  void Deactivate(void);

  const mrs_msgs::AttitudeCommand::ConstPtr update(const nav_msgs::Odometry::ConstPtr &odometry, const mrs_msgs::PositionCommand::ConstPtr &reference);
  const mrs_msgs::ControllerStatus::Ptr status();

  void dynamicReconfigureCallback(mrs_controllers::pid_gainsConfig &config, uint32_t level);

private:
  // --------------------------------------------------------------
  // |                     dynamic reconfigure                    |
  // --------------------------------------------------------------

  boost::recursive_mutex                      config_mutex_;
  typedef mrs_controllers::pid_gainsConfig    Config;
  typedef dynamic_reconfigure::Server<Config> ReconfigureServer;
  boost::shared_ptr<ReconfigureServer>        reconfigure_server_;
  void drs_callback(mrs_controllers::pid_gainsConfig &config, uint32_t level);
  mrs_controllers::pid_gainsConfig last_drs_config;

private:
  Pid *pid_x;
  Pid *pid_y;
  Pid *pid_z;

  double hover_thrust_;
  double roll, pitch, yaw;

  // gains
  double kpxy_, kixy_, kdxy_;
  double kpz_, kiz_, kdz_;

  double max_tilt_angle_;

  mrs_msgs::AttitudeCommand::ConstPtr last_output_command;

  ros::Time last_update;
  bool      first_iteration = true;
};

PidController::PidController(void) {
}

void PidController::dynamicReconfigureCallback(mrs_controllers::pid_gainsConfig &config, uint32_t level) {

  ROS_INFO("Controller gains were kpxy: %3.5f, kdxy: %3.5f, kixy: %3.5f, kpz: %3.5f, kdz: %3.5f, kiz: %3.5f", kpxy_, kdxy_, kixy_, kpz_, kdz_, kiz_);
  kpxy_         = config.kpxy;
  kdxy_         = config.kdxy;
  kixy_         = config.kixy;
  kpz_          = config.kpz;
  kdz_          = config.kdz;
  kiz_          = config.kiz;
  hover_thrust_ = config.hover_thrust;
  ROS_INFO("Controller gains ARE kpxy: %3.5f, kdxy: %3.5f, kixy: %3.5f, kpz: %3.5f, kdz: %3.5f, kiz: %3.5f", kpxy_, kdxy_, kixy_, kpz_, kdz_, kiz_);

  pid_x->setGains(kpxy_, kdxy_, kixy_);
  pid_y->setGains(kpxy_, kdxy_, kixy_);
  pid_z->setGains(kpz_, kdz_, kiz_);
}

bool PidController::Activate(void) {

  first_iteration = true;

  ROS_INFO("The PidController was activated.");

  return true;
}

void PidController::Deactivate(void) {
}

void PidController::Initialize(const ros::NodeHandle &parent_nh) {

  ros::NodeHandle priv_nh(parent_nh, "pid_controller");

  ros::Time::waitForValid();

  // --------------------------------------------------------------
  // |                       load parameters                      |
  // --------------------------------------------------------------

  priv_nh.param("kpxy", kpxy_, -1.0);
  priv_nh.param("kdxy", kdxy_, -1.0);
  priv_nh.param("kixy", kixy_, -1.0);
  priv_nh.param("kpz", kpz_, -1.0);
  priv_nh.param("kdz", kdz_, -1.0);
  priv_nh.param("kiz", kiz_, -1.0);
  priv_nh.param("hover_thrust", hover_thrust_, -1.0);

  if (kpxy_ < 0) {
    ROS_ERROR("PidController: kpxy is not specified!");
    ros::shutdown();
  }

  if (kdxy_ < 0) {
    ROS_ERROR("PidController: kdxy is not specified!");
    ros::shutdown();
  }

  if (kixy_ < 0) {
    ROS_ERROR("PidController: kixy is not specified!");
    ros::shutdown();
  }

  if (kpz_ < 0) {
    ROS_ERROR("PidController: kpz is not specified!");
    ros::shutdown();
  }

  if (kdz_ < 0) {
    ROS_ERROR("PidController: kdz is not specified!");
    ros::shutdown();
  }

  if (kiz_ < 0) {
    ROS_ERROR("PidController: kiz is not specified!");
    ros::shutdown();
  }

  if (hover_thrust_ < 0) {
    ROS_ERROR("PidController: hover_thrust is not specified!");
    ros::shutdown();
  }

  priv_nh.param("max_tilt_angle", max_tilt_angle_, -1.0);
  if (max_tilt_angle_ < 0) {
    ROS_ERROR("PidController: max_tilt_angle is not specified!");
    ros::shutdown();
  }

  // convert to radians
  max_tilt_angle_ = (max_tilt_angle_ / 360) * 2 * 3.1459;

  ROS_INFO("PidController was launched with gains:");
  ROS_INFO("horizontal: kpxy: %3.5f, kdxy: %3.5f, kixy: %3.5f", kpxy_, kdxy_, kixy_);
  ROS_INFO("vertical:   kpz: %3.5f, kdz: %3.5f, kiz: %3.5f", kpz_, kdz_, kiz_);

  // --------------------------------------------------------------
  // |                       initialize pids                      |
  // --------------------------------------------------------------

  pid_x = new Pid("x", kpxy_, kdxy_, kixy_, 0.1, max_tilt_angle_, 0.99);
  pid_y = new Pid("y", kpxy_, kdxy_, kixy_, 0.1, max_tilt_angle_, 0.99);
  pid_z = new Pid("z", kpz_, kdz_, kiz_, 0.1, 1.0, 0.99);

  // --------------------------------------------------------------
  // |                     dynamic reconfigure                    |
  // --------------------------------------------------------------

  last_drs_config.kpxy         = kpxy_;
  last_drs_config.kdxy         = kdxy_;
  last_drs_config.kixy         = kixy_;
  last_drs_config.kpz          = kpz_;
  last_drs_config.kdz          = kdz_;
  last_drs_config.kiz          = kiz_;
  last_drs_config.hover_thrust = hover_thrust_;

  reconfigure_server_.reset(new ReconfigureServer(config_mutex_, priv_nh));
  reconfigure_server_->updateConfig(last_drs_config);
  ReconfigureServer::CallbackType f = boost::bind(&PidController::dynamicReconfigureCallback, this, _1, _2);
  reconfigure_server_->setCallback(f);

}

const mrs_msgs::AttitudeCommand::ConstPtr PidController::update(const nav_msgs::Odometry::ConstPtr &       odometry,
                                                                const mrs_msgs::PositionCommand::ConstPtr &reference) {

  // --------------------------------------------------------------
  // |                  calculate control errors                  |
  // --------------------------------------------------------------

  double error_x = reference->position.x - odometry->pose.pose.position.x;
  double error_y = reference->position.y - odometry->pose.pose.position.y;
  double error_z = reference->position.z - odometry->pose.pose.position.z;

  // --------------------------------------------------------------
  // |                      calculate the dt                      |
  // --------------------------------------------------------------

  double dt;

  if (first_iteration) {

    pid_x->reset(error_x);
    pid_y->reset(error_y);
    pid_z->reset(error_z);
    last_update = ros::Time::now();

    ROS_INFO("PidController: first iteration, reseting pids");

    first_iteration = false;

    return mrs_msgs::AttitudeCommand::Ptr();

  } else {

    dt          = (ros::Time::now() - last_update).toSec();
    last_update = ros::Time::now();
  }

  if (dt <= 0.001) {
    ROS_WARN("PidController: the update was called with too small dt!");
    return last_output_command;
  }

  // --------------------------------------------------------------
  // |                 calculate the euler angles                 |
  // --------------------------------------------------------------

  double         yaw, pitch, roll;
  tf::Quaternion quaternion_odometry;
  quaternionMsgToTF(odometry->pose.pose.orientation, quaternion_odometry);
  tf::Matrix3x3 m(quaternion_odometry);
  m.getRPY(roll, pitch, yaw);

  // --------------------------------------------------------------
  // |                     calculate the PIDs                     |
  // --------------------------------------------------------------

  double action_x = pid_x->update(error_x, dt);
  double action_y = pid_y->update(error_y, dt);
  double action_z = (pid_z->update(error_z, dt) + hover_thrust_) * (1 / ((cos(roll) * cos(pitch))));

  mrs_msgs::AttitudeCommand::Ptr output_command(new mrs_msgs::AttitudeCommand);
  output_command->header.stamp = ros::Time::now();

  /* ROS_INFO("yaw=%f", yaw); */

  output_command->pitch  = action_x * cos(yaw) - action_y * sin(yaw);
  output_command->roll   = action_y * cos(yaw) + action_x * sin(yaw);
  output_command->yaw    = reference->yaw;
  output_command->thrust = action_z;

  return output_command;
}

const mrs_msgs::ControllerStatus::Ptr PidController::status() {

  return mrs_msgs::ControllerStatus::Ptr();
}
}

#include <pluginlib/class_list_macros.h>
PLUGINLIB_EXPORT_CLASS(mrs_controllers::PidController, mrs_mav_manager::Controller)  //<reformat_checkpoint>
