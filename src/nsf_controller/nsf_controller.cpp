#define VERSION "0.0.3.0"

/* includes //{ */

#include <ros/ros.h>
#include <ros/package.h>

#include <dynamic_reconfigure/server.h>
#include <mrs_msgs/AttitudeCommand.h>
#include <nav_msgs/Odometry.h>
#include <tf/transform_datatypes.h>

#include <math.h>

#include <mrs_uav_manager/Controller.h>

#include <mrs_controllers/nsf_controllerConfig.h>

#include <mrs_lib/Profiler.h>
#include <mrs_lib/ParamLoader.h>
#include <mrs_lib/Utils.h>
#include <mrs_lib/mutex.h>

//}

#define X 0
#define Y 1
#define Z 2

namespace mrs_controllers
{

namespace nsf_controller
{

/* //{ class NsfController */

class NsfController : public mrs_uav_manager::Controller {

public:
  void initialize(const ros::NodeHandle &parent_nh, std::string name, std::string name_space, const mrs_uav_manager::MotorParams motor_params,
                  const double uav_mass, const double g, std::shared_ptr<mrs_uav_manager::CommonHandlers_t> common_handlers);
  bool activate(const mrs_msgs::AttitudeCommand::ConstPtr &cmd);
  void deactivate(void);

  const mrs_msgs::AttitudeCommand::ConstPtr update(const mrs_msgs::UavState::ConstPtr &uav_state, const mrs_msgs::PositionCommand::ConstPtr &reference);
  const mrs_msgs::ControllerStatus          getStatus();

  void dynamicReconfigureCallback(mrs_controllers::nsf_controllerConfig &config, uint32_t level);

  double calculateGainChange(const double current_value, const double desired_value, const bool bypass_rate, std::string name);

  Eigen::Vector2d rotate2d(const Eigen::Vector2d vector_in, double angle);

  virtual void switchOdometrySource(const mrs_msgs::UavState::ConstPtr &msg);

  void resetDisturbanceEstimators(void);

private:
  std::string _version_;

  bool is_initialized = false;
  bool is_active      = false;

  std::shared_ptr<mrs_uav_manager::CommonHandlers_t> common_handlers_;

private:
  mrs_msgs::UavState uav_state_;
  std::mutex         mutex_uav_state_;

  // --------------------------------------------------------------
  // |                     dynamic reconfigure                    |
  // --------------------------------------------------------------

  boost::recursive_mutex                        config_mutex_;
  typedef mrs_controllers::nsf_controllerConfig Config;
  typedef dynamic_reconfigure::Server<Config>   ReconfigureServer;
  boost::shared_ptr<ReconfigureServer>          reconfigure_server_;
  void                                          drs_callback(mrs_controllers::nsf_controllerConfig &config, uint32_t level);
  mrs_controllers::nsf_controllerConfig         drs_desired_gains;

private:
  double                       uav_mass_;
  double                       uav_mass_difference;
  double                       g_;
  mrs_uav_manager::MotorParams motor_params_;
  double                       hover_thrust;

  // actual gains (used and already filtered)
  double kpxy, kiwxy, kibxy, kvxy, kaxy;
  double kpz, kvz, kaz;
  double kiwxy_lim, kibxy_lim;
  double km, km_lim;

  // desired gains (set by DRS)
  std::mutex mutex_gains;
  std::mutex mutex_desired_gains;

  double max_tilt_angle_;
  double thrust_saturation_;

  mrs_msgs::AttitudeCommand::ConstPtr last_output_command;
  mrs_msgs::AttitudeCommand           activation_control_command_;

  ros::Time last_update;
  bool      first_iteration = true;

  bool   mute_lateral_gains               = false;
  bool   mutex_lateral_gains_after_toggle = false;
  double mute_coefficitent_;

private:
  mrs_lib::Profiler profiler;
  bool              profiler_enabled_ = false;

private:
  ros::Timer timer_gain_filter;
  void       timerGainsFilter(const ros::TimerEvent &event);

  int    gains_filter_timer_rate_;
  double gains_filter_change_rate_;
  double gains_filter_min_change_rate_;

  double gains_filter_max_change_;  // calculated from change_rate_/timer_rate_;
  double gains_filter_min_change_;  // calculated from change_rate_/timer_rate_;

private:
  Eigen::Vector2d Ib_b;  // body error integral in the body frame
  Eigen::Vector2d Iw_w;  // world error integral in the world_frame
  std::mutex      mutex_integrals_;
};

//}

// --------------------------------------------------------------
// |                   controller's interface                   |
// --------------------------------------------------------------

/* //{ initialize() */

void NsfController::initialize(const ros::NodeHandle &parent_nh, [[maybe_unused]] std::string name, std::string name_space,
                               const mrs_uav_manager::MotorParams motor_params, const double uav_mass, const double g,
                               std::shared_ptr<mrs_uav_manager::CommonHandlers_t> common_handlers) {

  ros::NodeHandle nh_(parent_nh, name_space);

  common_handlers_ = common_handlers;

  ros::Time::waitForValid();

  this->motor_params_ = motor_params;
  this->uav_mass_     = uav_mass;
  this->g_            = g;

  // --------------------------------------------------------------
  // |                       load parameters                      |
  // --------------------------------------------------------------

  mrs_lib::ParamLoader param_loader(nh_, "NsfController");

  param_loader.load_param("version", _version_);

  if (_version_ != VERSION) {

    ROS_ERROR("[NsfController]: the version of the binary (%s) does not match the config file (%s), please build me!", VERSION, _version_.c_str());
    ros::shutdown();
  }

  param_loader.load_param("enable_profiler", profiler_enabled_);

  // lateral gains
  param_loader.load_param("default_gains/horizontal/kp", kpxy);
  param_loader.load_param("default_gains/horizontal/kv", kvxy);
  param_loader.load_param("default_gains/horizontal/ka", kaxy);

  param_loader.load_param("default_gains/horizontal/kiw", kiwxy);
  param_loader.load_param("default_gains/horizontal/kib", kibxy);

  param_loader.load_param("lateral_mute_coefficitent", mute_coefficitent_);

  // height gains
  param_loader.load_param("default_gains/vertical/kp", kpz);
  param_loader.load_param("default_gains/vertical/kv", kvz);
  param_loader.load_param("default_gains/vertical/ka", kaz);

  // mass estimator
  param_loader.load_param("default_gains/weight_estimator/km", km);
  param_loader.load_param("default_gains/weight_estimator/km_lim", km_lim);

  // integrator limits
  param_loader.load_param("default_gains/horizontal/kiw_lim", kiwxy_lim);
  param_loader.load_param("default_gains/horizontal/kib_lim", kibxy_lim);

  // constraints
  param_loader.load_param("max_tilt_angle", max_tilt_angle_);
  param_loader.load_param("thrust_saturation", thrust_saturation_);

  // gain filtering
  param_loader.load_param("gains_filter/filter_rate", gains_filter_timer_rate_);
  param_loader.load_param("gains_filter/perc_change_rate", gains_filter_change_rate_);
  param_loader.load_param("gains_filter/min_change_rate", gains_filter_min_change_rate_);

  gains_filter_max_change_ = gains_filter_change_rate_ / gains_filter_timer_rate_;
  gains_filter_min_change_ = gains_filter_min_change_rate_ / gains_filter_timer_rate_;

  if (!param_loader.loaded_successfully()) {
    ROS_ERROR("[NsfController]: Could not load all parameters!");
    ros::shutdown();
  }

  // convert to radians
  max_tilt_angle_ = (max_tilt_angle_ / 180) * M_PI;

  uav_mass_difference = 0;
  Iw_w                = Eigen::Vector2d::Zero(2);
  Ib_b                = Eigen::Vector2d::Zero(2);

  // --------------------------------------------------------------
  // |                 calculate the hover thrust                 |
  // --------------------------------------------------------------

  hover_thrust = sqrt(uav_mass_ * g_) * motor_params_.hover_thrust_a + motor_params_.hover_thrust_b;

  // --------------------------------------------------------------
  // |                     dynamic reconfigure                    |
  // --------------------------------------------------------------

  drs_desired_gains.kpxy      = kpxy;
  drs_desired_gains.kvxy      = kvxy;
  drs_desired_gains.kaxy      = kaxy;
  drs_desired_gains.kiwxy     = kiwxy;
  drs_desired_gains.kibxy     = kibxy;
  drs_desired_gains.kpz       = kpz;
  drs_desired_gains.kvz       = kvz;
  drs_desired_gains.kaz       = kaz;
  drs_desired_gains.kiwxy_lim = kiwxy_lim;
  drs_desired_gains.kibxy_lim = kibxy_lim;
  drs_desired_gains.km        = km;
  drs_desired_gains.km_lim    = km_lim;

  reconfigure_server_.reset(new ReconfigureServer(config_mutex_, nh_));
  reconfigure_server_->updateConfig(drs_desired_gains);
  ReconfigureServer::CallbackType f = boost::bind(&NsfController::dynamicReconfigureCallback, this, _1, _2);
  reconfigure_server_->setCallback(f);

  // --------------------------------------------------------------
  // |                          profiler                          |
  // --------------------------------------------------------------

  profiler = mrs_lib::Profiler(nh_, "NsfController", profiler_enabled_);

  // --------------------------------------------------------------
  // |                           timers                           |
  // --------------------------------------------------------------

  timer_gain_filter = nh_.createTimer(ros::Rate(gains_filter_timer_rate_), &NsfController::timerGainsFilter, this);

  // | ----------------------- finish init ---------------------- |

  if (!param_loader.loaded_successfully()) {
    ROS_ERROR("[NsfController]: Could not load all parameters!");
    ros::shutdown();
  }

  ROS_INFO("[NsfController]: initialized, version %s", VERSION);

  is_initialized = true;
}

//}

/* //{ activate() */

bool NsfController::activate(const mrs_msgs::AttitudeCommand::ConstPtr &cmd) {

  if (cmd == mrs_msgs::AttitudeCommand::Ptr()) {

    ROS_WARN("[NsfController]: activated without getting the last controller's command.");

    return false;

  } else {

    activation_control_command_ = *cmd;
    uav_mass_difference         = cmd->mass_difference;

    activation_control_command_.controller_enforcing_constraints = false;

    Ib_b[0] = asin(cmd->disturbance_bx_b / (g_ * cmd->total_mass));
    Ib_b[1] = asin(cmd->disturbance_by_b / (g_ * cmd->total_mass));

    Iw_w[0] = asin(cmd->disturbance_wx_w / (g_ * cmd->total_mass));
    Iw_w[1] = asin(cmd->disturbance_wy_w / (g_ * cmd->total_mass));

    ROS_INFO(
        "[NsfController]: setting the mass difference and disturbances from the last AttitudeCmd: mass difference: %.2f kg, Ib_b: %.2f, %.2f N, Iw_w: %.2f, "
        "%.2f N",
        uav_mass_difference, cmd->disturbance_bx_b, cmd->disturbance_by_b, cmd->disturbance_wx_w, cmd->disturbance_wx_w);

    ROS_INFO("[NsfController]: activated with a last controller's command.");
  }

  first_iteration = true;

  ROS_INFO("[NsfController]: activated");

  is_active = true;

  return true;
}

//}

/* //{ deactivate() */

void NsfController::deactivate(void) {

  is_active           = false;
  first_iteration     = false;
  uav_mass_difference = 0;

  ROS_INFO("[NsfController]: deactivated");
}

//}

/* //{ update() */

const mrs_msgs::AttitudeCommand::ConstPtr NsfController::update(const mrs_msgs::UavState::ConstPtr &       uav_state,
                                                                const mrs_msgs::PositionCommand::ConstPtr &reference) {

  mrs_lib::Routine profiler_routine = profiler.createRoutine("update");

  {
    std::scoped_lock lock(mutex_uav_state_);

    uav_state_ = *uav_state;
  }

  if (!is_active) {
    return mrs_msgs::AttitudeCommand::ConstPtr();
  }

  // --------------------------------------------------------------
  // |          load the control reference and estimates          |
  // --------------------------------------------------------------

  // Rp - position reference in global frame
  // Rp - velocity reference in global frame
  Eigen::Vector3d Rp(reference->position.x, -reference->position.y, reference->position.z);
  Eigen::Vector3d Rv(reference->velocity.x, -reference->velocity.y, reference->velocity.z);

  // Op - position in global frame
  // Op - velocity in global frame
  Eigen::Vector3d Op(uav_state->pose.position.x, -uav_state->pose.position.y, uav_state->pose.position.z);
  Eigen::Vector3d Ov(uav_state->velocity.linear.x, -uav_state->velocity.linear.y, uav_state->velocity.linear.z);

  // --------------------------------------------------------------
  // |                  calculate control errors                  |
  // --------------------------------------------------------------

  Eigen::Vector3d Ep = Rp - Op;
  Eigen::Vector3d Ev = Rv - Ov;

  // --------------------------------------------------------------
  // |                      calculate the dt                      |
  // --------------------------------------------------------------

  double dt;

  if (first_iteration) {

    last_update = uav_state->header.stamp;

    first_iteration = false;

    return mrs_msgs::AttitudeCommand::ConstPtr(new mrs_msgs::AttitudeCommand(activation_control_command_));

  } else {

    dt          = (uav_state->header.stamp - last_update).toSec();
    last_update = uav_state->header.stamp;
  }

  if (fabs(dt) <= 0.001) {

    ROS_DEBUG("[NsfController]: the last odometry message came too close! %f", dt);

    if (last_output_command != mrs_msgs::AttitudeCommand::Ptr()) {

      return last_output_command;

    } else {

      return mrs_msgs::AttitudeCommand::ConstPtr(new mrs_msgs::AttitudeCommand(activation_control_command_));
    }
  }

  // --------------------------------------------------------------
  // |                 calculate the euler angles                 |
  // --------------------------------------------------------------

  double         yaw, pitch, roll;
  tf::Quaternion uav_attitude;
  quaternionMsgToTF(uav_state->pose.orientation, uav_attitude);
  tf::Matrix3x3 m(uav_attitude);
  m.getRPY(roll, pitch, yaw);

  // --------------------------------------------------------------
  // |                recalculate the hover thrust                |
  // --------------------------------------------------------------

  double total_mass = uav_mass_ + uav_mass_difference;

  hover_thrust = sqrt(total_mass * g_) * motor_params_.hover_thrust_a + motor_params_.hover_thrust_b;

  // --------------------------------------------------------------
  // |                      update parameters                     |
  // --------------------------------------------------------------

  if (mute_lateral_gains && !reference->disable_position_gains) {
    mutex_lateral_gains_after_toggle = true;
  }
  mute_lateral_gains = reference->disable_position_gains;

  // --------------------------------------------------------------
  // |                     calculate the NSFs                     |
  // --------------------------------------------------------------

  Eigen::Vector2d Ib_w = rotate2d(Ib_b, -yaw);

  // create vectors of gains
  Eigen::Vector3d kp, kv, ka;

  {
    std::scoped_lock lock(mutex_gains);

    kp << kpxy, kpxy, kpz;
    kv << kvxy, kvxy, kvz;
    ka << kaxy, kaxy, kaz;
  }

  // calculate the feed forwared acceleration
  Eigen::Vector3d feed_forward(asin((reference->acceleration.x * cos(pitch) * cos(roll)) / g_),
                               asin((-reference->acceleration.y * cos(pitch) * cos(roll)) / g_), reference->acceleration.z * (hover_thrust / g_));

  // | -------- calculate the componentes of our feedback ------- |
  Eigen::Vector3d p_component, v_component, a_component, i_component;

  p_component = kp.cwiseProduct(Ep);
  v_component = kv.cwiseProduct(Ev);
  a_component = ka.cwiseProduct(feed_forward);
  {
    std::scoped_lock lock(mutex_integrals_);

    i_component << Ib_w + Iw_w, Eigen::VectorXd::Zero(1, 1);
  }

  Eigen::Vector3d feedback_w = (p_component + v_component + a_component + i_component + Eigen::Vector3d(0, 0, hover_thrust))
                                   .cwiseProduct(Eigen::Vector3d(1, 1, 1 / (cos(roll) * cos(pitch))));

  // --------------------------------------------------------------
  // |                  validation and saturation                 |
  // --------------------------------------------------------------

  // | ------------ validate and saturate the X and Y components ------------- |

  // check the world Y controller
  double x_saturated = false;
  if (!std::isfinite(feedback_w[X])) {
    feedback_w[X] = 0;
    ROS_ERROR_THROTTLE(1.0, "[NsfController]: NaN detected in variable \"feedback_w[X]\", setting it to 0!!!");
  } else if (feedback_w[X] > max_tilt_angle_) {
    feedback_w[X] = max_tilt_angle_;
    x_saturated   = true;
  } else if (feedback_w[X] < -max_tilt_angle_) {
    feedback_w[X] = -max_tilt_angle_;
    x_saturated   = true;
  }

  // check the world Y controller
  double y_saturated = false;
  if (!std::isfinite(feedback_w[Y])) {
    feedback_w[Y] = 0;
    ROS_ERROR_THROTTLE(1.0, "[NsfController]: NaN detected in variable \"feedback_w[Y]\", setting it to 0!!!");
  } else if (feedback_w[Y] > max_tilt_angle_) {
    feedback_w[Y] = max_tilt_angle_;
    y_saturated   = true;
  } else if (feedback_w[Y] < -max_tilt_angle_) {
    feedback_w[Y] = -max_tilt_angle_;
    y_saturated   = true;
  }

  // | ---------------- validate the Z component ---------------- |

  // check the world Y controller
  double z_saturated = false;
  if (!std::isfinite(feedback_w[Z])) {
    feedback_w[Z] = 0;
    ROS_ERROR_THROTTLE(1.0, "[NsfController]: NaN detected in variable \"feedback_w[Z]\", setting it to 0!!!");
  } else if (feedback_w[Z] > thrust_saturation_) {
    feedback_w[Z] = thrust_saturation_;
    z_saturated   = true;
    ROS_WARN("[NsfController]: saturating thrust to %.2f", thrust_saturation_);
  } else if (feedback_w[Z] < 0.0) {
    feedback_w[Z] = 0;
    z_saturated   = true;
    ROS_WARN("[NsfController]: saturating thrust to %.2f", 0.0);
  }

  if (x_saturated) {
    ROS_WARN_THROTTLE(1.0, "[NsfController]: X is saturated");
  }

  if (y_saturated) {
    ROS_WARN_THROTTLE(1.0, "[NsfController]: Y is saturated");
  }

  if (z_saturated) {
    ROS_WARN_THROTTLE(1.0, "[NsfController]: Z is saturated");
  }

  // --------------------------------------------------------------
  // |                  integrate the world error                 |
  // --------------------------------------------------------------

  {
    std::scoped_lock lock(mutex_gains, mutex_integrals_);

    Eigen::Vector3d integration_switch(1, 1, 0);

    if (x_saturated && mrs_lib::sign(feedback_w[X]) == mrs_lib::sign(Ep[X])) {
      integration_switch[X] = 0;
    }

    if (y_saturated && mrs_lib::sign(feedback_w[Y]) == mrs_lib::sign(Ep[Y])) {
      integration_switch[Y] = 0;
    }

    // integrate the world error
    Iw_w += kiwxy * (Ep.cwiseProduct(integration_switch)).head(2) * dt;

    // saturate the world
    double world_integral_saturated = false;
    if (!std::isfinite(Iw_w[0])) {
      Iw_w[0] = 0;
      ROS_ERROR_THROTTLE(1.0, "[NsfController]: NaN detected in variable \"Iw_w[0]\", setting it to 0!!!");
    } else if (Iw_w[0] > kiwxy_lim) {
      Iw_w[0]                  = kiwxy_lim;
      world_integral_saturated = true;
    } else if (Iw_w[0] < -kiwxy_lim) {
      Iw_w[0]                  = -kiwxy_lim;
      world_integral_saturated = true;
    }

    if (kiwxy_lim >= 0 && world_integral_saturated) {
      ROS_WARN_THROTTLE(1.0, "[NsfController]: NSF's world X integral is being saturated!");
    }

    // saturate the world
    world_integral_saturated = false;
    if (!std::isfinite(Iw_w[1])) {
      Iw_w[1] = 0;
      ROS_ERROR_THROTTLE(1.0, "[NsfController]: NaN detected in variable \"Iw_w[1]\", setting it to 0!!!");
    } else if (Iw_w[1] > kiwxy_lim) {
      Iw_w[1]                  = kiwxy_lim;
      world_integral_saturated = true;
    } else if (Iw_w[1] < -kiwxy_lim) {
      Iw_w[1]                  = -kiwxy_lim;
      world_integral_saturated = true;
    }

    if (kiwxy_lim >= 0 && world_integral_saturated) {
      ROS_WARN_THROTTLE(1.0, "[NsfController]: NSF's world Y integral is being saturated!");
    }
  }

  // --------------------------------------------------------------
  // |                  integrate the body error                 |
  // --------------------------------------------------------------

  {
    std::scoped_lock lock(mutex_gains);

    // rotate the control errors to the body
    Eigen::Vector2d Ep_body = rotate2d(Ep.head(2), yaw);

    // integrate the body error
    Ib_b += kibxy * Ep_body * dt;

    // saturate the body
    double body_integral_saturated = false;
    if (!std::isfinite(Ib_b[0])) {
      Ib_b[0] = 0;
      ROS_ERROR_THROTTLE(1.0, "[NsfController]: NaN detected in variable \"Ib_b[0]\", setting it to 0!!!");
    } else if (Ib_b[0] > kibxy_lim) {
      Ib_b[0]                 = kibxy_lim;
      body_integral_saturated = true;
    } else if (Ib_b[0] < -kibxy_lim) {
      Ib_b[0]                 = -kibxy_lim;
      body_integral_saturated = true;
    }

    if (kibxy_lim > 0 && body_integral_saturated) {
      ROS_WARN_THROTTLE(1.0, "[NsfController]: NSF's body pitch integral is being saturated!");
    }

    // saturate the body
    body_integral_saturated = false;
    if (!std::isfinite(Ib_b[1])) {
      Ib_b[1] = 0;
      ROS_ERROR_THROTTLE(1.0, "[NsfController]: NaN detected in variable \"Ib_b[1]\", setting it to 0!!!");
    } else if (Ib_b[1] > kibxy_lim) {
      Ib_b[1]                 = kibxy_lim;
      body_integral_saturated = true;
    } else if (Ib_b[1] < -kibxy_lim) {
      Ib_b[1]                 = -kibxy_lim;
      body_integral_saturated = true;
    }

    if (kibxy_lim > 0 && body_integral_saturated) {
      ROS_WARN_THROTTLE(1.0, "[NsfController]: NSF's body roll integral is being saturated!");
    }
  }

  // --------------------------------------------------------------
  // |                integrate the mass difference               |
  // --------------------------------------------------------------

  {
    std::scoped_lock lock(mutex_gains);

    if (!z_saturated) {
      uav_mass_difference += km * Ep[2] * dt;
    }

    // saturate the mass estimator
    bool uav_mass_saturated = false;
    if (!std::isfinite(uav_mass_difference)) {
      uav_mass_difference = 0;
      ROS_WARN_THROTTLE(1.0, "[NsfController]: NaN detected in variable \"uav_mass_difference\", setting it to 0 and returning!!!");
    } else if (uav_mass_difference > km_lim) {
      uav_mass_difference = km_lim;
      uav_mass_saturated  = true;
    } else if (uav_mass_difference < -km_lim) {
      uav_mass_difference = -km_lim;
      uav_mass_saturated  = true;
    }

    if (uav_mass_saturated) {
      ROS_WARN_THROTTLE(1.0, "[NsfController]: The uav_mass_difference is being saturated to %1.3f!", uav_mass_difference);
    }
  }

  // --------------------------------------------------------------
  // |            report on the values of the integrals           |
  // --------------------------------------------------------------

  {
    std::scoped_lock lock(mutex_integrals_);

    // report in the internal representation of the disturbance -> tilt angle
    double rad_deg = 180.0 / M_PI;

    ROS_INFO_THROTTLE(5.0, "[NsfController]: disturbance in the tilt represenation");
    ROS_INFO_THROTTLE(5.0, "[NsfController]: world error integral: x %1.2f deg, y %1.2f deg, lim: %1.2f deg", rad_deg * Iw_w[X], rad_deg * Iw_w[Y],
                      rad_deg * kiwxy_lim);
    ROS_INFO_THROTTLE(5.0, "[NsfController]: body error integral:  x %1.2f deg, y %1.2f deg, lim: %1.2f deg", rad_deg * Ib_b[X], rad_deg * Ib_b[Y],
                      rad_deg * kibxy_lim);

    // report in the more universal representation -> force
    double hover_force = total_mass * g_;

    ROS_INFO_THROTTLE(5.0, "[NsfController]: disturbance in the force represenation");
    ROS_INFO_THROTTLE(5.0, "[NsfController]: world error integral: x %1.2f N, y %1.2f N, lim: %1.2f N", hover_force * sin(Iw_w[X]), hover_force * sin(Iw_w[Y]),
                      hover_force * sin(kiwxy_lim));
    ROS_INFO_THROTTLE(5.0, "[NsfController]: body error integral:  x %1.2f N, y %1.2f N, lim: %1.2f N", hover_force * sin(Ib_b[X]), hover_force * sin(Ib_b[Y]),
                      hover_force * sin(kibxy_lim));
  }

  // --------------------------------------------------------------
  // |                 produce the control output                 |
  // --------------------------------------------------------------

  mrs_msgs::AttitudeCommand::Ptr output_command(new mrs_msgs::AttitudeCommand);
  output_command->header.stamp = ros::Time::now();

  // rotate the feedback to the body frame
  Eigen::Vector2d feedback_b = rotate2d(feedback_w.head(2), yaw);

  output_command->euler_attitude.x   = feedback_b[1];
  output_command->euler_attitude.y   = feedback_b[0];
  output_command->euler_attitude.z   = reference->yaw;  // TODO this will not work with custom heading estimator
  output_command->euler_attitude_set = true;

  output_command->quater_attitude_set = false;
  output_command->attitude_rate_set   = false;

  output_command->thrust = feedback_w[2];

  output_command->mode_mask = output_command->MODE_EULER_ATTITUDE;

  output_command->mass_difference = uav_mass_difference;
  output_command->total_mass      = total_mass;

  output_command->disturbance_bx_b = g_ * total_mass * sin(Ib_b[0]);
  output_command->disturbance_by_b = g_ * total_mass * sin(Ib_b[1]);

  output_command->disturbance_bx_w = g_ * total_mass * sin(Ib_w[0]);
  output_command->disturbance_by_w = g_ * total_mass * sin(Ib_w[1]);

  output_command->disturbance_wx_w = g_ * total_mass * sin(Iw_w[0]);
  output_command->disturbance_wy_w = g_ * total_mass * sin(Iw_w[1]);

  output_command->controller_enforcing_constraints = false;

  output_command->controller = "NsfController";

  last_output_command = output_command;

  return output_command;
}

//}

/* //{ getStatus() */

const mrs_msgs::ControllerStatus NsfController::getStatus() {

  mrs_msgs::ControllerStatus controller_status;

  controller_status.active = is_active;

  return controller_status;
}

//}

/* switchOdometrySource() //{ */

void NsfController::switchOdometrySource(const mrs_msgs::UavState::ConstPtr &msg) {

  ROS_INFO("[NfsController]: switching the odometry source");

  auto uav_state = mrs_lib::get_mutexed(mutex_uav_state_, uav_state_);

  // | ----- transform world disturabances to the new frame ----- |

  geometry_msgs::Vector3Stamped world_integrals;

  world_integrals.header.stamp    = ros::Time::now();
  world_integrals.header.frame_id = uav_state.header.frame_id;

  world_integrals.vector.x = Iw_w[0];
  world_integrals.vector.y = Iw_w[1];
  world_integrals.vector.z = 0;

  auto res = common_handlers_->transformer->transformSingle(msg->header.frame_id, world_integrals);

  if (res) {

    std::scoped_lock lock(mutex_integrals_);

    Iw_w[0] = res.value().vector.x;
    Iw_w[1] = res.value().vector.y;
  } else {

    ROS_ERROR_THROTTLE(1.0, "[NfsController]: could not transform world integral to the new frame");

    std::scoped_lock lock(mutex_integrals_);

    Iw_w[0] = 0;
    Iw_w[1] = 0;
  }
}

//}

/* resetDisturbanceEstimators() //{ */

void NsfController::resetDisturbanceEstimators(void) {

  std::scoped_lock lock(mutex_integrals_);

  Iw_w = Eigen::Vector2d::Zero(2);
  Ib_b = Eigen::Vector2d::Zero(2);
}

//}

// --------------------------------------------------------------
// |                          callbacks                         |
// --------------------------------------------------------------

/* //{ dynamicReconfigureCallback() */

void NsfController::dynamicReconfigureCallback(mrs_controllers::nsf_controllerConfig &config, [[maybe_unused]] uint32_t level) {

  {
    std::scoped_lock lock(mutex_desired_gains);

    drs_desired_gains = config;
  }

  ROS_INFO("[NsfController]: DRS updated gains");
}

//}

// --------------------------------------------------------------
// |                           timers                           |
// --------------------------------------------------------------

/* timerGainFilter() //{ */

void NsfController::timerGainsFilter(const ros::TimerEvent &event) {

  mrs_lib::Routine profiler_routine = profiler.createRoutine("timerGainsFilter", gains_filter_timer_rate_, 0.05, event);

  double gain_coeff                = 1;
  bool   bypass_filter             = mute_lateral_gains || mutex_lateral_gains_after_toggle;
  mutex_lateral_gains_after_toggle = false;

  if (mute_lateral_gains) {
    gain_coeff = mute_coefficitent_;
  }

  // calculate the difference
  {
    std::scoped_lock lock(mutex_gains, mutex_desired_gains);

    kpxy      = calculateGainChange(kpxy, drs_desired_gains.kpxy * gain_coeff, bypass_filter, "kpxy");
    kvxy      = calculateGainChange(kvxy, drs_desired_gains.kvxy * gain_coeff, bypass_filter, "kvxy");
    kaxy      = calculateGainChange(kaxy, drs_desired_gains.kaxy * gain_coeff, bypass_filter, "kaxy");
    kiwxy     = calculateGainChange(kiwxy, drs_desired_gains.kiwxy * gain_coeff, bypass_filter, "kiwxy");
    kibxy     = calculateGainChange(kibxy, drs_desired_gains.kibxy * gain_coeff, bypass_filter, "kibxy");
    kpz       = calculateGainChange(kpz, drs_desired_gains.kpz, false, "kpz");
    kvz       = calculateGainChange(kvz, drs_desired_gains.kvz, false, "kvz");
    kaz       = calculateGainChange(kaz, drs_desired_gains.kaz, false, "kaz");
    km        = calculateGainChange(km, drs_desired_gains.km, false, "km");
    kiwxy_lim = calculateGainChange(kiwxy_lim, drs_desired_gains.kiwxy_lim, false, "kiwxy_lim");
    kibxy_lim = calculateGainChange(kibxy_lim, drs_desired_gains.kibxy_lim, false, "kibxy_lim");
    km_lim    = calculateGainChange(km_lim, drs_desired_gains.km_lim, false, "km_lim");
  }
}

//}

// --------------------------------------------------------------
// |                       other routines                       |
// --------------------------------------------------------------

/* calculateGainChange() //{ */

double NsfController::calculateGainChange(const double current_value, const double desired_value, const bool bypass_rate, std::string name) {

  double change = desired_value - current_value;

  if (!bypass_rate) {

    // if current value is near 0...
    double change_in_perc;
    double saturated_change;

    if (fabs(current_value) < 1e-6) {
      change *= gains_filter_max_change_;
    } else {

      saturated_change = change;

      change_in_perc = (current_value + saturated_change) / current_value - 1.0;

      if (change_in_perc > gains_filter_max_change_) {
        saturated_change = current_value * gains_filter_max_change_;
      } else if (change_in_perc < -gains_filter_max_change_) {
        saturated_change = current_value * -gains_filter_max_change_;
      }

      if (fabs(saturated_change) < fabs(change) * gains_filter_min_change_) {
        change *= gains_filter_min_change_;
      } else {
        change = saturated_change;
      }
    }
  }

  if (fabs(change) > 1e-3) {
    ROS_INFO_THROTTLE(1.0, "[NsfController]: changing gain \"%s\" from %f to %f", name.c_str(), current_value, desired_value);
  }

  return current_value + change;
}

//}

/* rotate2d() //{ */

Eigen::Vector2d NsfController::rotate2d(const Eigen::Vector2d vector_in, double angle) {

  Eigen::Rotation2D<double> rot2(angle);

  return rot2.toRotationMatrix() * vector_in;
}

//}

}  // namespace nsf_controller

}  // namespace mrs_controllers

#include <pluginlib/class_list_macros.h>
PLUGINLIB_EXPORT_CLASS(mrs_controllers::nsf_controller::NsfController, mrs_uav_manager::Controller)
