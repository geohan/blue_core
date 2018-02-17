#include <ros/ros.h>
#include <hardware_interface/joint_command_interface.h>
#include <hardware_interface/joint_state_interface.h>
#include <hardware_interface/robot_hw.h>
#include <urdf/model.h>
#include <sensor_msgs/JointState.h>
#include <std_msgs/Float64.h>
#include <ros/node_handle.h>
#include <vector>
#include <string>
#include <math.h>

#include <joint_limits_interface/joint_limits.h>
#include <joint_limits_interface/joint_limits_urdf.h>
#include <joint_limits_interface/joint_limits_rosparam.h>
#include <transmission_interface/simple_transmission.h>
#include <transmission_interface/differential_transmission.h>
#include <transmission_interface/transmission_interface.h>

using namespace transmission_interface;

class KokoHW: public hardware_interface::RobotHW
{

public:
  KokoHW(ros::NodeHandle &nh)
  {
    int y = 1000;
    ROS_ERROR("%d", y++);
    if (!nh.getParam("koko_hardware/joint_names", joint_names)) {
      ROS_INFO("No koko_hardware/joint_names given (namespace: %s)", nh.getNamespace().c_str());
    }
    if (!nh.getParam("koko_hardware/motor_names", motor_names)) {
      ROS_INFO("No koko_hardware/motor_names given (namespace: %s)", nh.getNamespace().c_str());
    }
    if (!nh.getParam("koko_hardware/gear_ratio", gear_ratios)) {
      ROS_INFO("No koko_hardware/gear_ratio given (namespace: %s)", nh.getNamespace().c_str());
    }
    if (!nh.getParam("koko_hardware/directions", directions)) {
      ROS_INFO("No koko_hardware/directions given (namespace: %s)", nh.getNamespace().c_str());
    }
    if (!nh.getParam("koko_hardware/torque_directions", torque_directions)) {
      ROS_INFO("No koko_hardware/torque_directions given (namespace: %s)", nh.getNamespace().c_str());
    }
    if (!nh.getParam("koko_hardware/current_slope", current_slope)) {
      ROS_INFO("No koko_hardware/current_slope given (namespace: %s)", nh.getNamespace().c_str());
    }
    if (!nh.getParam("koko_hardware/current_offset", current_offset)) {
      ROS_INFO("No koko_hardware/current_offset given (namespace: %s)", nh.getNamespace().c_str());
    }
    if (!nh.getParam("koko_hardware/paired_constraints", paired_constraints)) {
      ROS_INFO("No koko_hardware/paired_constraints given (namespace: %s", nh.getNamespace().c_str());
    }
    if (paired_constraints.size() % 2 != 0) {
      ROS_INFO("Paired_constraints length must be even");
    }
    if (!nh.getParam("koko_hardware/hardstop_torque_limit", hardstop_torque_limit)) {
      ROS_INFO("No koko_hardware/hardstop_torque_limit given (namespace: %s)", nh.getNamespace().c_str());
    }

    num_joints = joint_names.size();

    // loading in joint limits
    //https://github.com/ros-controls/ros_control/wiki/joint_limits_interface
    boost::shared_ptr<urdf::ModelInterface> koko_urdf;
    joint_limits_interface::JointLimits limits;
    ROS_ERROR("getting joint limits");
    for (int j; j< num_joints; j ++){
      boost::shared_ptr<const urdf::Joint> urdf_joint = koko_urdf->getJoint(joint_names[j]);
      const bool urdf_limits_ok = getJointLimits(urdf_joint, limits);
      min_angles[j] = limits.min_position;
      max_angles[j] = limits.max_position;
      ROS_ERROR("min: %f, max: %f", min_angles[j], max_angles[j]);
    }

    // resizing vectors to number of joints of the robot
    motor_pos.resize(num_joints, 0.0);
    motor_vel.resize(num_joints, 0.0);
    cmd.resize(num_joints, 0.0);
    pos.resize(num_joints, 0.0);
    vel.resize(num_joints, 0.0);
    eff.resize(num_joints, 0.0);
    joint_state_initial.resize(num_joints, 0.0);
    angle_after_calibration.resize(num_joints, 0.0);

    jnt_cmd_publishers.resize(num_joints);

    for (int i = 0; i < num_joints; i++) {
      cmd[i] = 0.0;
      hardware_interface::JointStateHandle state_handle_a(joint_names[i], &pos[i], &vel[i], &eff[i]);
      jnt_state_interface.registerHandle(state_handle_a);
      ROS_INFO("joint %s", joint_names[i].c_str());
    }
    registerInterface(&jnt_state_interface);

    for (int i = 0; i < num_joints; i++) {
      hardware_interface::JointHandle effort_handle_a(jnt_state_interface.getHandle(joint_names[i]), &cmd[i]);
      jnt_effort_interface.registerHandle(effort_handle_a);
    }
    registerInterface(&jnt_effort_interface);

    position_read = 0;
    calibrated = 0;
    prev_is_calibrated = 0;
    is_calibrated = 0;

    for (int i = 0; i < num_joints; i++) {
      joint_state_initial[i] = 0.0;
    }

    calibration_num = 10;

    jnt_state_tracker_subscriber = nh.subscribe("joint_state_tracker", 1000, &KokoHW::CalibrateJointState, this);
    jnt_state_subscriber = nh.subscribe("koko_hardware/motor_states", 1000, &KokoHW::UpdateJointState, this);

    for (int i = 0; i < motor_names.size(); i++) {
      jnt_cmd_publishers[i] = nh.advertise<std_msgs::Float64>("koko_hardware/" + motor_names[i] + "_cmd", 1000);
      ROS_INFO("Publishers %s", motor_names[i].c_str());
    }

    //*******************************************************************************************************************// added for hardware interaface
    // TODO fill in parameters from read in
    int j_idx = 0;
    base_trans = new SimpleTransmission(gear_ratios[j_idx], 1.0);
    j_idx ++;

    std::vector<double> shoulder_gear_ratios(2, 1.0);
    shoulder_gear_ratios[0] = gear_ratios[j_idx];
    j_idx ++;
    shoulder_gear_ratios[1] = gear_ratios[j_idx];
    j_idx ++;
    shoulder_trans = new DifferentialTransmission(shoulder_gear_ratios, std::vector<double>(2, 1.0));

    std::vector<double> upper_arm_gear_ratios(2, 1.0);
    upper_arm_gear_ratios[0] = gear_ratios[j_idx];
    j_idx ++;
    upper_arm_gear_ratios[1] = gear_ratios[j_idx];
    j_idx ++;
    upper_arm_trans = new DifferentialTransmission(upper_arm_gear_ratios, std::vector<double>(2, 1.0));

    std::vector<double> wrist_gear_ratios(2, 1.0);
    wrist_gear_ratios[0] = gear_ratios[j_idx];
    j_idx ++;
    wrist_gear_ratios[1] = gear_ratios[j_idx];
    j_idx ++;
    wrist_trans = new DifferentialTransmission(wrist_gear_ratios, std::vector<double>(2, 1.0));

    // Wrap base simple transmission raw data - current state
    a_state_data[0].position.push_back(&a_curr_pos[0]);
    a_state_data[0].velocity.push_back(&a_curr_vel[0]);
    a_state_data[0].effort.push_back(&a_curr_eff[0]);

    j_state_data[0].position.push_back(&j_curr_pos[0]);
    j_state_data[0].velocity.push_back(&j_curr_vel[0]);
    j_state_data[0].effort.push_back(&j_curr_eff[0]);

    // Wrap differential transmission raw data - current state
    for(int k = 1; k < 4 ; k++ ) {
      a_state_data[k].position.push_back(&a_curr_pos[k * 2 - 1]);
      a_state_data[k].position.push_back(&a_curr_pos[k * 2]);
      a_state_data[k].velocity.push_back(&a_curr_vel[k * 2 - 1]);
      a_state_data[k].velocity.push_back(&a_curr_vel[k * 2]);
      a_state_data[k].effort.push_back(&a_curr_eff[k * 2 - 1]);
      a_state_data[k].effort.push_back(&a_curr_eff[k * 2]);

      j_state_data[k].position.push_back(&j_curr_pos[k * 2 - 1]);
      j_state_data[k].position.push_back(&j_curr_pos[k * 2]);
      j_state_data[k].velocity.push_back(&j_curr_vel[k * 2 - 1]);
      j_state_data[k].velocity.push_back(&j_curr_vel[k * 2]);
      j_state_data[k].effort.push_back(&j_curr_eff[k * 2 - 1]);
      j_state_data[k].effort.push_back(&j_curr_eff[k * 2]);

    }
    // Wrap simple transmission raw data - position command
    a_cmd_data[0].effort.push_back(&a_cmd_eff[0]);
    j_cmd_data[0].effort.push_back(&j_cmd_eff[0]);

    // Wrap differential transmission raw data - position command
    for(int k = 1; k < 4 ; k++ ) {
      a_cmd_data[k].effort.push_back(&a_cmd_eff[k * 2 - 1]);
      a_cmd_data[k].effort.push_back(&a_cmd_eff[k * 2]);
      j_cmd_data[k].effort.push_back(&j_cmd_eff[k * 2 - 1]);
      j_cmd_data[k].effort.push_back(&j_cmd_eff[k * 2]);
    }
    // ...once the raw data has been wrapped, the rest is straightforward //////////////////////////////////////////////

    // Register transmissions to each interface
    act_to_jnt_state.registerHandle(ActuatorToJointStateHandle("base_trans",
                                                               base_trans,
                                                               a_state_data[0],
                                                               j_state_data[0]));

    act_to_jnt_state.registerHandle(ActuatorToJointStateHandle("shoulder_trans",
                                                              shoulder_trans,
                                                              a_state_data[1],
                                                              j_state_data[1]));

    act_to_jnt_state.registerHandle(ActuatorToJointStateHandle("upper_arm_trans",
                                                              upper_arm_trans,
                                                              a_state_data[2],
                                                              j_state_data[2]));

    act_to_jnt_state.registerHandle(ActuatorToJointStateHandle("wrist_trans",
                                                              wrist_trans,
                                                              a_state_data[3],
                                                              j_state_data[3]));

    jnt_to_act_eff.registerHandle(JointToActuatorEffortHandle("base_trans",
                                                                base_trans,
                                                                a_cmd_data[0],
                                                                j_cmd_data[0]));

    jnt_to_act_eff.registerHandle(JointToActuatorEffortHandle("shoulder_trans",
                                                                shoulder_trans,
                                                                a_cmd_data[1],
                                                                j_cmd_data[1]));

    jnt_to_act_eff.registerHandle(JointToActuatorEffortHandle("upper_arm_trans",
                                                                upper_arm_trans,
                                                                a_cmd_data[2],
                                                                j_cmd_data[2]));

    jnt_to_act_eff.registerHandle(JointToActuatorEffortHandle("wrist_trans",
                                                                wrist_trans,
                                                                a_cmd_data[3],
                                                                j_cmd_data[3]));
  }

  void UpdateJointState(const sensor_msgs::JointState::ConstPtr& msg) {
    for (int i = 0; i < msg->name.size(); i++) {
      int index = -1;
      for (int j = 0; j < motor_names.size(); j++) {
        if (msg->name[i].compare(motor_names[j]) == 0){
          index = j;
        }
      }
      if (index == -1){
        ROS_ERROR("Some Joint koko_hwi error, msg name %s, with %d joints", msg->name[i].c_str(), num_joints);
      }

      if (is_calibrated != 1) {
        angle_after_calibration[index] = msg->position[i];
      } else if (is_calibrated == 1){

        motor_pos[index] = msg->position[i] - angle_after_calibration[index];
        motor_vel[index] = msg->velocity[i];
        double pre_pos = msg->position[i] - angle_after_calibration[index];
        double pre_vel = msg->velocity[i];
        if(std::find(paired_constraints.begin(), paired_constraints.end(), index) != paired_constraints.end()) {
          ROS_INFO("f10");
          int a = std::find(paired_constraints.begin(), paired_constraints.end(), index) - paired_constraints.begin();
          //trying to set index a
          if (a % 2 == 0) {
            int b = a + 1;
            pre_pos = -.5 * motor_pos[paired_constraints[a]] + .5 * motor_pos[paired_constraints[b]];
            pre_vel = -.5 * motor_vel[paired_constraints[a]] + .5 * motor_vel[paired_constraints[b]];
          } else {
            int b = a - 1;
            pre_pos = .5 * motor_pos[paired_constraints[b]] + .5 * motor_pos[paired_constraints[a]];
            pre_vel = .5 * motor_vel[paired_constraints[b]] + .5 * motor_vel[paired_constraints[a]];
          }
        }
        pos[index] = directions[index] * (pre_pos / gear_ratios[index]) + joint_state_initial[index];
        vel[index] = directions[index] * pre_vel / gear_ratios[index];
        eff[index] = torque_directions[index] * msg->effort[i] * gear_ratios[index];
        position_read = 1;

        // added for using transmission interface
        a_curr_pos[index] = msg->position[i] - angle_after_calibration[index];
        a_curr_vel[index] = msg->velocity[i];
        a_curr_eff[index] = msg->effort[i];
        // update state of all motors
      }
    }

    // added for using transmission interface
    act_to_jnt_state.propagate();
    for(int i = 0; i < num_joints; i ++) {
      pos[i] = j_curr_pos[i];
      vel[i] = j_curr_pos[i];
      eff[i] = j_curr_pos[i];
    }
    //  update all positions of joints

  }

  void CalibrateJointState(const sensor_msgs::JointState::ConstPtr& msg) {
    if (calibrated == calibration_num)
    {
      for (int i = 0; i < joint_state_initial.size(); i++) {
        joint_state_initial[i] = msg->position[i];
        pos[i] = joint_state_initial[i];
        ROS_INFO("calibrated initial joint state %f", joint_state_initial[i]);
      }
      calibrated++;
      is_calibrated = 1;
      ROS_INFO("Finished Calibrating Joint States");
    }
    else if(calibrated != calibration_num + 1)
    {
      for (int i = 0; i < joint_state_initial.size(); i++) {
        joint_state_initial[i] = msg->position[i];
        pos[i] = joint_state_initial[i];
        ROS_INFO("calibrated initial joint state %f", joint_state_initial[i]);
      }
      calibrated++;
    }
  }

  double convertMotorTorqueToCurrent(double motor_torque, int index) {
    return current_slope[index] * motor_torque + current_offset[index];
  }

  ros::Time get_time() {
    return ros::Time::now();
  }

  ros::Duration get_period() {
    ros::Time current_time = ros::Time::now();
    ros::Duration period = current_time - last_time;
    last_time = current_time;
    return period;
  }

  void read() {
  }

  void write() {
    PublishJointCommand();
  }

  void PublishJointCommand() {

    if (!is_calibrated) {
      return;
    }

    std::vector<double> pre(num_joints);
    std::vector<double> cmd_oriented(num_joints);
    for (int k = 0; k < num_joints; k++) {
      pre[k] = cmd[k];
      cmd_oriented[k] = torque_directions[k] * cmd[k];

      // making sure robot is not trying to push past joint limits
      double current_angle = pos[k];
      if ( (current_angle < min_angles[k] && cmd_oriented[k] < 0) || (current_angle > max_angles[k] && cmd_oriented[k] > 0) ){
          if ( abs(cmd_oriented[k]) > hardstop_torque_limit) {
            cmd_oriented[k] = hardstop_torque_limit;
          }
      }
    }

    for (int j = 0; j < paired_constraints.size(); j = j + 2) {
      int index_a = paired_constraints[j];
      int index_b = paired_constraints[j + 1];
      pre[index_a] = -0.5 * cmd_oriented[index_a] + 0.5 * cmd_oriented[index_b];
      pre[index_b] =  0.5 * cmd_oriented[index_a] + 0.5 * cmd_oriented[index_b];
    }

    for (int i = 0; i < num_joints; i++) {
      double motor_torque =  pre[i] / gear_ratios[i];
      double motor_current = convertMotorTorqueToCurrent(motor_torque, i);
      std_msgs::Float64 commandMsg;
      commandMsg.data =  motor_current;
      jnt_cmd_publishers[i].publish(commandMsg);
    }

    // ********************************************************************* //
    // added for using transmission interface
    for (int i = 0; i < num_joints; i++){
      a_cmd_eff[i] = cmd[i];
    }
    //propoate through transmission
    jnt_to_act_eff.propagate();
    for (int i = 0; i < num_joints; i++) {
      double motor_torque = j_cmd_eff[i];
      double motor_current = convertMotorTorqueToCurrent(motor_torque, i);
      std_msgs::Float64 commandMsg;
      commandMsg.data =  motor_current;
      jnt_cmd_publishers[i].publish(commandMsg);
    }
    // done for using transmission interface
    // ********************************************************************* //
  }

  const int getPositionRead() {
    return position_read;
  }

private:
  hardware_interface::JointStateInterface jnt_state_interface;
  hardware_interface::EffortJointInterface jnt_effort_interface;

  ros::Subscriber jnt_state_subscriber;
  std::vector<ros::Publisher> jnt_cmd_publishers;
  ros::Subscriber jnt_state_tracker_subscriber;

  ros::Time last_time;
  std::vector<std::string> joint_names;
  std::vector<std::string> motor_names;

  std::vector<double> gear_ratios;
  std::vector<double> cmd;
  std::vector<double> pos;
  std::vector<double> vel;
  std::vector<double> eff;
  std::vector<double> min_angles;
  std::vector<double> max_angles;


  std::vector<double> current_slope;
  std::vector<double> current_offset;
  std::vector<int> paired_constraints;

  std::vector<double> motor_pos;
  std::vector<double> motor_vel;

  int position_read;
  int calibrated;
  std::vector<double> joint_state_initial;
  std::vector<double> directions;
  std::vector<double> torque_directions;
  double hardstop_torque_limit;
  double i_to_T_slope;
  double i_to_T_intercept;
  int calibration_num;
  std::vector<double> angle_after_calibration;
  int is_calibrated;
  int prev_is_calibrated;

  int num_joints;

  // adding transmissions
  // Transmission interfaces
  ActuatorToJointStateInterface act_to_jnt_state; // For motor to joint state
  JointToActuatorEffortInterface jnt_to_act_eff; // For joint eff to actuator

  //For arbitrary length
  //SimpleTransmission simple_transmissions[];
  //DifferentialTransmission differential_transmissions[];

  // Transmissions
  SimpleTransmission *base_trans;
  DifferentialTransmission *shoulder_trans;
  DifferentialTransmission *upper_arm_trans;
  DifferentialTransmission *wrist_trans;

  //Actuator and joint space variables
  ActuatorData a_state_data[4];
  ActuatorData a_cmd_data[4];

  JointData j_state_data[4];
  JointData j_cmd_data[4];

  // Actuator and joint variables
  double a_curr_pos[7];
  double a_curr_vel[7];
  double a_curr_eff[7];
  double a_cmd_eff[7];

  double j_curr_pos[7];
  double j_curr_vel[7];
  double j_curr_eff[7];
  double j_cmd_eff[7];
};
