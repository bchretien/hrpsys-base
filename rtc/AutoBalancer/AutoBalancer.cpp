// -*- C++ -*-
/*!
 * @file  AutoBalancer.cpp
 * @brief autobalancer component
 * $Date$
 *
 * $Id$
 */

#include <rtm/CorbaNaming.h>
#include <hrpModel/Link.h>
#include <hrpModel/Sensor.h>
#include <hrpModel/ModelLoaderUtil.h>
#include "AutoBalancer.h"
#include <hrpModel/JointPath.h>
#include <hrpUtil/MatrixSolvers.h>

#ifdef __QNX__
using std::exp;
using std::fabs;
#else
using std::isnan;
using std::isinf;
#endif

#define MAX_TRANSITION_COUNT (2/m_dt)
typedef coil::Guard<coil::Mutex> Guard;
using namespace rats;

// Module specification
// <rtc-template block="module_spec">
static const char* autobalancer_spec[] =
    {
        "implementation_id", "AutoBalancer",
        "type_name",         "AutoBalancer",
        "description",       "autobalancer component",
        "version",           "1.0",
        "vendor",            "AIST",
        "category",          "example",
        "activity_type",     "DataFlowComponent",
        "max_instance",      "10",
        "language",          "C++",
        "lang_type",         "compile",
        // Configuration variables
        "conf.default.debugLevel", "0",
        ""
    };
// </rtc-template>

AutoBalancer::AutoBalancer(RTC::Manager* manager)
    : RTC::DataFlowComponentBase(manager),
      // <rtc-template block="initializer">
      m_qRefIn("qRef", m_qRef),
      m_qOut("q", m_q),
      m_AutoBalancerServicePort("AutoBalancerService"),
      // </rtc-template>
      m_robot(hrp::BodyPtr()),
      m_debugLevel(0),
      dummy(0)
{
    m_service0.autobalancer(this);
}

AutoBalancer::~AutoBalancer()
{
}


RTC::ReturnCode_t AutoBalancer::onInitialize()
{
    std::cout << "AutoBalancer::onInitialize()" << std::endl;
    bindParameter("debugLevel", m_debugLevel, "0");

    // Registration: InPort/OutPort/Service
    // <rtc-template block="registration">
    // Set InPort buffers
    addInPort("qRef", m_qRefIn);

    // Set OutPort buffer
    addOutPort("q", m_qOut);
  
    // Set service provider to Ports
    m_AutoBalancerServicePort.registerProvider("service0", "AutoBalancerService", m_service0);
  
    // Set service consumers to Ports
  
    // Set CORBA Service Ports
    addPort(m_AutoBalancerServicePort);
  
    // </rtc-template>
    // <rtc-template block="bind_config">
    // Bind variables and configuration variable
  
    // </rtc-template>

    RTC::Properties& prop =  getProperties();
    coil::stringTo(m_dt, prop["dt"].c_str());

    m_robot = hrp::BodyPtr(new hrp::Body());
    RTC::Manager& rtcManager = RTC::Manager::instance();
    std::string nameServer = rtcManager.getConfig()["corba.nameservers"];
    int comPos = nameServer.find(",");
    if (comPos < 0){
        comPos = nameServer.length();
    }
    nameServer = nameServer.substr(0, comPos);
    RTC::CorbaNaming naming(rtcManager.getORB(), nameServer.c_str());
    if (!loadBodyFromModelLoader(m_robot, prop["model"].c_str(), 
                                 CosNaming::NamingContext::_duplicate(naming.getRootContext())
                                 )){
        std::cerr << "failed to load model[" << prop["model"] << "]" 
                  << std::endl;
        return RTC::RTC_ERROR;
    }

    // allocate memory for outPorts
    m_qRef.data.length(m_robot->numJoints());
    m_q.data.length(m_robot->numJoints());
    qorg.resize(m_robot->numJoints());

    transition_count = 0;
    control_mode = MODE_IDLE;
    loop = 0;
    std::vector<hrp::Vector3> leg_pos;
    hrp::Vector3 leg_offset;
    coil::vstring leg_offset_str = coil::split(prop["abc_leg_offset"], ",");
    if (leg_offset_str.size() > 0) {
      for (size_t i = 0; i < 3; i++) coil::stringTo(leg_offset(i), leg_offset_str[i].c_str());
      std::cerr << "OFFSET " << leg_offset(0) << " " << leg_offset(1) << " " << leg_offset(2) << std::endl;
      leg_pos.push_back(hrp::Vector3(-1*leg_offset));
      leg_pos.push_back(hrp::Vector3(leg_offset));

      gg = ggPtr(new rats::gait_generator(m_dt, leg_pos, 1e-3*150/*[m]*/, 1e-3*50/*[m]*/, 10/*[deg]*/));
      gg_is_walking = gg_ending = gg_solved = false;
      std::vector<hrp::Vector3> dzo;
      dzo.push_back(hrp::Vector3::Zero());
      dzo.push_back(hrp::Vector3::Zero());
      gg->set_default_zmp_offsets(dzo);
    }
    fix_leg_coords = coordinates();

    return RTC::RTC_OK;
}



RTC::ReturnCode_t AutoBalancer::onFinalize()
{
  return RTC::RTC_OK;
}

/*
  RTC::ReturnCode_t AutoBalancer::onStartup(RTC::UniqueId ec_id)
  {
  return RTC::RTC_OK;
  }
*/

/*
  RTC::ReturnCode_t AutoBalancer::onShutdown(RTC::UniqueId ec_id)
  {
  return RTC::RTC_OK;
  }
*/

RTC::ReturnCode_t AutoBalancer::onActivated(RTC::UniqueId ec_id)
{
    std::cout << "AutoBalancer::onActivated(" << ec_id << ")" << std::endl;
    
    return RTC::RTC_OK;
}

/*
  RTC::ReturnCode_t AutoBalancer::onDeactivated(RTC::UniqueId ec_id)
  {
  return RTC::RTC_OK;
  }
*/

#define DEBUGP ((m_debugLevel==1 && loop%200==0) || m_debugLevel > 1 )
//#define DEBUGP2 ((loop%200==0))
#define DEBUGP2 (false)
RTC::ReturnCode_t AutoBalancer::onExecute(RTC::UniqueId ec_id)
{
  // std::cerr << "AutoBalancer::onExecute(" << ec_id << ")" << std::endl;
    loop ++;
    if (m_qRefIn.isNew()) {
        m_qRefIn.read();
    }
    Guard guard(m_mutex);
    robotstateOrg2qRef();
    if (control_mode == MODE_ABC ) solveLimbIK();
    if ( m_q.data.length() != 0 ) { // initialized
      for ( int i = 0; i < m_robot->numJoints(); i++ ){
        m_q.data[i] = m_robot->joint(i)->q;
      }
      m_qOut.write();
      if ( DEBUGP ) {
        std::cerr << "q     : ";
        for ( int i = 0; i < m_q.data.length(); i++ ){
          std::cerr << " " << m_q.data[i];
        }
        std::cerr << std::endl;
      }
    }
    return RTC::RTC_OK;
}

void AutoBalancer::robotstateOrg2qRef()
{
  base_pos_org = m_robot->rootLink()->p;
  base_rot_org = m_robot->rootLink()->R;
  for ( int i = 0; i < m_robot->numJoints(); i++ ){
    qorg[i] = m_robot->joint(i)->q;
    m_robot->joint(i)->q = m_qRef.data[i];
  }
  m_robot->calcForwardKinematics();
  if ( ikp.size() > 0 ) {
    coordinates tmp_fix_coords;
    if ( gg_is_walking ) {
      //gg->set_default_zmp_offsets(tmpzo);
      gg_solved = gg->proc_one_tick(rats::gait_generator::CYCLOID);
      ikp[gg->get_support_leg()].target_p0 = gg->get_support_leg_coords().pos;
      ikp[gg->get_support_leg()].target_r0 = gg->get_support_leg_coords().rot;
      ikp[gg->get_swing_leg()].target_p0 = gg->get_swing_leg_coords().pos;
      ikp[gg->get_swing_leg()].target_r0 = gg->get_swing_leg_coords().rot;
      gg->get_swing_support_mid_coords(tmp_fix_coords);
    } else {
      tmp_fix_coords = fix_leg_coords;
    }
    fixLegToCoords(":both", tmp_fix_coords);
    target_base_pos = m_robot->rootLink()->p;
    target_base_rot = m_robot->rootLink()->R;
    if (!gg_is_walking) {
      for ( std::map<std::string, ABCIKparam>::iterator it = ikp.begin(); it != ikp.end(); it++ ) {
        it->second.target_p0 = m_robot->link(it->second.target_name)->p;
        it->second.target_r0 = m_robot->link(it->second.target_name)->R;
      }
    }
    if (gg_is_walking) {
      target_com = gg->get_cog();
    } else {
      //coordinates rc(target_coords[":rleg"]), lc(target_coords[":lleg"]);
      //rc.translate(tmpzo[0]); /* :rleg */
      //lc.translate(tmpzo[1]); /* :lleg */
      //target_cog = (rc.pos + lc.pos) / 2.0;
      //target_com = hrp::Vector3::Zero();
      target_com = (m_robot->link(ikp[":rleg"].target_name)->p+
                    m_robot->link(ikp[":lleg"].target_name)->p)/2.0;
    }
  }
  if ( transition_count > 0 ) {
    for ( std::map<std::string, ABCIKparam>::iterator it = ikp.begin(); it != ikp.end(); it++ ) {
      hrp::JointPathExPtr manip = it->second.manip;
      for ( int j = 0; j < manip->numJoints(); j++ ) {
        int i = manip->joint(j)->jointId; // index in robot model
        hrp::Link* joint =  m_robot->joint(i);
        // transition_smooth_gain moves from 0 to 1
        // (/ (log (/ (- 1 0.99) 0.99)) 0.5)
        double transition_smooth_gain = 1/(1+exp(-9.19*(((MAX_TRANSITION_COUNT - transition_count) / MAX_TRANSITION_COUNT) - 0.5)));
        joint->q = ( m_qRef.data[i] - transition_joint_q[i] ) * transition_smooth_gain + transition_joint_q[i];
      }
    }
    transition_count--;
    if(transition_count <= 0){ // erase impedance param
      std::cerr << "Finished cleanup" << std::endl;
      control_mode = MODE_IDLE;
    }
  }
}

void AutoBalancer::fixLegToCoords (const std::string& leg, const coordinates& coords)
{
  coordinates tar, ref, delta, tmp;
  coordinates rleg_endcoords(m_robot->link(ikp[":rleg"].target_name)->p,
                             m_robot->link(ikp[":rleg"].target_name)->R);
  coordinates lleg_endcoords(m_robot->link(ikp[":lleg"].target_name)->p,
                             m_robot->link(ikp[":lleg"].target_name)->R);
  mid_coords(tar, 0.5, rleg_endcoords , lleg_endcoords);
  tmp = coords;
  ref = coordinates(m_robot->rootLink()->p, m_robot->rootLink()->R);
  tar.transformation(delta, ref, ":local");
  tmp.transform(delta, ":local");
  m_robot->rootLink()->p = tmp.pos;
  m_robot->rootLink()->R = tmp.rot;
  m_robot->calcForwardKinematics();
}

bool AutoBalancer::solveLimbIKforLimb (ABCIKparam& param, const double transition_smooth_gain, const double cog_gain)
{
  hrp::Link* base = m_robot->link(param.base_name);
  hrp::Link* target = m_robot->link(param.target_name);
  assert(target);
  assert(base);
  param.current_p0 = target->p;
  param.current_r0 = target->R;

  hrp::JointPathExPtr manip = param.manip;
  assert(manip);
  const int n = manip->numJoints();
  hrp::dmatrix J(6, n);
  hrp::dmatrix Jinv(n, 6);
  hrp::dmatrix Jnull(n, n);

  manip->calcJacobianInverseNullspace(J, Jinv, Jnull);
  //manip->calcInverseKinematics2Loop(vel_p, vel_r, dq);

  hrp::Vector3 vel_p, vel_r;
  vel_p = param.target_p0 - param.current_p0;
  rats::difference_rotation(vel_r, param.current_r0, param.target_r0);
  vel_p *= 1 * cog_gain;
  vel_r *= 1 * cog_gain;
  if ( transition_count < 0 ) {
    vel_p = vel_p * transition_smooth_gain;
    vel_r = vel_r * transition_smooth_gain;
  }

  hrp::dvector v(6);
  v << vel_p, vel_r;
  if (DEBUGP) {
    rats::print_vector(std::cerr, vel_r);
  }
  hrp::dvector dq(n);
  dq = Jinv * v; // dq = pseudoInverse(J) * v

  // dq = J#t a dx + ( I - J# J ) Jt b dx
  // avoid-nspace-joint-limit: avoiding joint angle limit
  //
  // dH/dq = (((t_max + t_min)/2 - t) / ((t_max - t_min)/2)) ^2
  hrp::dvector u(n);
  for(int j=0; j < n; ++j) { u[j] = 0; }
  for ( int j = 0; j < n ; j++ ) {
    double jang = manip->joint(j)->q;
    double jmax = manip->joint(j)->ulimit;
    double jmin = manip->joint(j)->llimit;
    double r = ((( (jmax + jmin) / 2.0) - jang) / ((jmax - jmin) / 2.0));
    if ( r > 0 ) { r = r*r; } else { r = - r*r; }
    u[j] += r;
  }
  if ( DEBUGP ) {
    std::cerr << "    u : " << u;
    std::cerr << "  dqb : " << Jnull * u;
  }
  if ( transition_count < 0 ) {
    u = u * transition_smooth_gain;
  }
  dq = dq + Jnull * ( 0.001 *  u );
  //
  // qref - qcurr
  for(int j=0; j < n; ++j) { u[j] = 0; }
  for ( int j = 0; j < manip->numJoints(); j++ ) {
    u[j] = ( m_qRef.data[manip->joint(j)->jointId] - manip->joint(j)->q );
  }
  if ( transition_count < 0 ) {
    u = u * transition_smooth_gain;
  }
  dq = dq + Jnull * ( 0.01 *  u );

  // break if dq(j) is nan/nil
  bool dq_check = true;
  for(int j=0; j < n; ++j){
    if ( std::isnan(dq(j)) || std::isinf(dq(j)) ) {
      dq_check = false;
      break;
    }
  }
  if ( ! dq_check ) return false;

  // check max speed
  double max_speed = 0;
  for(int j=0; j < n; ++j){
    max_speed = std::max(max_speed, fabs(dq(j)));
  }
  if ( max_speed > 0.2*0.5 ) { // 0.5 safety margin
    if ( DEBUGP ) {
      std::cerr << "spdlmt: ";
      for(int j=0; j < n; ++j) { std::cerr << dq(j) << " "; } std::cerr << std::endl;
    }
    for(int j=0; j < n; ++j) {
      dq(j) = dq(j) * 0.2*0.5 / max_speed;
    }
    if ( DEBUGP ) {
      std::cerr << "spdlmt: ";
      for(int j=0; j < n; ++j) { std::cerr << dq(j) << " "; } std::cerr << std::endl;
    }
  }

  // update robot model
  for(int j=0; j < n; ++j){
    manip->joint(j)->q += dq(j);
  }


  // check limit
  for(int j=0; j < n; ++j){
    if ( manip->joint(j)->q > manip->joint(j)->ulimit) {
      std::cerr << "Upper joint limit error " << manip->joint(j)->name << std::endl;
      manip->joint(j)->q = manip->joint(j)->ulimit;
    }
    if ( manip->joint(j)->q < manip->joint(j)->llimit) {
      std::cerr << "Lower joint limit error " << manip->joint(j)->name << std::endl;
      manip->joint(j)->q = manip->joint(j)->llimit;
    }
    manip->joint(j)->q = std::max(manip->joint(j)->q, manip->joint(j)->llimit);
  }

  manip->calcForwardKinematics();
  return true;
}

void AutoBalancer::solveLimbIK ()
{
  double transition_smooth_gain = 1.0;
  if ( transition_count < 0 ) {
    // (/ (log (/ (- 1 0.99) 0.99)) 0.5)
    transition_smooth_gain = 1/(1+exp(-9.19*(((MAX_TRANSITION_COUNT + transition_count) / MAX_TRANSITION_COUNT) - 0.5)));
  }

  for ( std::map<std::string, ABCIKparam>::iterator it = ikp.begin(); it != ikp.end(); it++ ) {
    for ( int j = 0; j < it->second.manip->numJoints(); j++ ){
      int i = it->second.manip->joint(j)->jointId;
      m_robot->joint(i)->q = qorg[i];
    }
  }
  m_robot->rootLink()->p = base_pos_org;
  m_robot->rootLink()->R = base_rot_org;
  m_robot->calcForwardKinematics();
  hrp::Vector3 current_com = m_robot->calcCM();
  hrp::Vector3 dif_com = current_com - target_com;
  dif_com(2) = m_robot->rootLink()->p(2) - target_base_pos(2);
  double cog_gain = 1 * transition_smooth_gain;
  m_robot->rootLink()->p = m_robot->rootLink()->p + cog_gain * -0.05 * dif_com;
  coordinates tmpc;
  mid_coords(tmpc, 0.9,
             coordinates(m_robot->rootLink()->p, base_rot_org),
             coordinates(m_robot->rootLink()->p, target_base_rot));
  m_robot->rootLink()->R = tmpc.rot;
  m_robot->calcForwardKinematics();

  for ( std::map<std::string, ABCIKparam>::iterator it = ikp.begin(); it != ikp.end(); it++ ) {
    if (!solveLimbIKforLimb(it->second, transition_smooth_gain, cog_gain)) break;
  }
  if ( transition_count < 0 ) {
    transition_count++;
  }
  //if (gg_is_walking && !gg_solved) stopWalking ();
}

/*
  RTC::ReturnCode_t AutoBalancer::onAborting(RTC::UniqueId ec_id)
  {
  return RTC::RTC_OK;
  }
*/

/*
  RTC::ReturnCode_t AutoBalancer::onError(RTC::UniqueId ec_id)
  {
  return RTC::RTC_OK;
  }
*/

/*
  RTC::ReturnCode_t AutoBalancer::onReset(RTC::UniqueId ec_id)
  {
  return RTC::RTC_OK;
  }
*/

/*
  RTC::ReturnCode_t AutoBalancer::onStateUpdate(RTC::UniqueId ec_id)
  {
  return RTC::RTC_OK;
  }
*/

/*
  RTC::ReturnCode_t AutoBalancer::onRateChanged(RTC::UniqueId ec_id)
  {
  return RTC::RTC_OK;
  }
*/

void AutoBalancer::startABCparam(const OpenHRP::AutoBalancerService::AutoBalancerLimbParamSequence& alp)
{
  std::cerr << "abc start" << std::endl;
  transition_count = -MAX_TRANSITION_COUNT; // when start impedance, count up to 0
  Guard guard(m_mutex);

  for (size_t i = 0; i < alp.length(); i++) {
    const OpenHRP::AutoBalancerService::AutoBalancerLimbParam& tmpalp = alp[i];
    ABCIKparam tmp;
    tmp.base_name = std::string(tmpalp.base_name);
    tmp.target_name = std::string(tmpalp.target_name);
    tmp.manip = hrp::JointPathExPtr(new hrp::JointPathEx(m_robot, m_robot->link(tmp.base_name), m_robot->link(tmp.target_name)));
    ikp.insert(std::pair<std::string, ABCIKparam>(std::string(tmpalp.name), tmp));
    std::cerr << "ABC " << std::string(tmpalp.name) << " " << std::string(tmpalp.base_name) << " " << std::string(tmpalp.target_name) << std::endl;
  }

  for ( int i = 0; i < m_robot->numJoints(); i++ ){
    m_robot->joint(i)->q = m_qRef.data[i];
  }
  m_robot->calcForwardKinematics();
  fixLegToCoords(":both", fix_leg_coords);
  base_pos_org = m_robot->rootLink()->p;
  base_rot_org = m_robot->rootLink()->R;
  control_mode = MODE_ABC;
}

void AutoBalancer::stopABCparam()
{
  std::cerr << "abc stop" << std::endl;
  Guard guard(m_mutex);
  mid_coords(fix_leg_coords, 0.5,
             coordinates(ikp[":rleg"].target_p0, ikp[":rleg"].target_r0),
             coordinates(ikp[":lleg"].target_p0, ikp[":lleg"].target_r0));
  transition_count = MAX_TRANSITION_COUNT; // when start impedance, count up to 0
  transition_joint_q.resize(m_robot->numJoints());
  for (int i = 0; i < m_robot->numJoints(); i++ ) {
    transition_joint_q[i] = m_robot->joint(i)->q;
  }
  control_mode = MODE_SYNC;
  gg_ending = gg_solved = gg_is_walking = false;
}

void AutoBalancer::startWalking ()
{
  hrp::Vector3 cog(m_robot->calcCM());
  std::string init_support_leg (gg->get_footstep_front_leg() == ":rleg" ? ":lleg" : ":rleg");
  std::string init_swing_leg (gg->get_footstep_front_leg());
  coordinates spc, swc;
  gg->initialize_gait_parameter(cog,
                                coordinates(ikp[init_support_leg].target_p0, ikp[init_support_leg].target_r0),
                                coordinates(ikp[init_swing_leg].target_p0, ikp[init_swing_leg].target_r0));
  while ( !gg->proc_one_tick(rats::gait_generator::CYCLOID) );
  gg_is_walking = gg_solved = true;
  gg_ending = false;
}

void AutoBalancer::stopWalking ()
{
  if (!gg_ending){
    //gg_ending = true; // tmpolary
    /* sync */
  } else {
    /* overwrite sequencer's angle-vector when finishing steps */
    gg_is_walking = false;
    //rb->get_foot_midcoords(fix_leg_coords);
    gg->clear_footstep_node_list();
    // coordinates rc, lc;
    // rb->get_end_coords(rc, ":rleg");
    // rb->get_end_coords(lc, ":lleg");
    // rc.translate(dzo[0]); /* :rleg */
    // lc.translate(dzo[1]); /* :lleg */
    // target_cog = (rc.pos + lc.pos) / 2.0;
    /* sync */
    gg_ending = false;
  }
}

bool AutoBalancer::startABC (const OpenHRP::AutoBalancerService::AutoBalancerLimbParamSequence& alp)
{
  if (control_mode == MODE_IDLE) {
    std::cerr << "START ABC" << std::endl;
    startABCparam(alp);
    return true;
  } else {
    return false;
  }
}

bool AutoBalancer::stopABC ()
{
  if (control_mode == MODE_ABC) {
    std::cerr << "STOP ABC" << std::endl;
    stopABCparam();
    return true;
  } else {
    return false;
  }
}

bool AutoBalancer::goPos(const double& x, const double& y, const double& th)
{
  if (control_mode == MODE_ABC ) {
    coordinates foot_midcoords;
    mid_coords(foot_midcoords, 0.5,
               coordinates(ikp[":rleg"].target_p0, ikp[":rleg"].target_r0),
               coordinates(ikp[":lleg"].target_p0, ikp[":lleg"].target_r0));
    gg->go_pos_param_2_footstep_list(x, y, th, foot_midcoords);
    gg->print_footstep_list();
    startWalking();
  }
  return 0;
}

bool AutoBalancer::goVelocity(const double& vx, const double& vy, const double& vth)
{
  if (control_mode == MODE_ABC ) {
    //    if (gg_is_walking) {
    if (gg_is_walking && gg_solved) {
      gg->set_velocity_param(vx, vy, vth);
    } else {
      coordinates foot_midcoords;
      mid_coords(foot_midcoords, 0.5,
                 coordinates(ikp[":rleg"].target_p0, ikp[":rleg"].target_r0),
                 coordinates(ikp[":lleg"].target_p0, ikp[":lleg"].target_r0));
      gg->initialize_velocity_mode(foot_midcoords, vx, vy, vth);
      startWalking();
    }
  }
  return 0;
}

bool AutoBalancer::goStop ()
{
  gg->finalize_velocity_mode();
  waitFootSteps();
  return true;
}

bool AutoBalancer::setFootSteps(const OpenHRP::AutoBalancerService::FootstepSequence& fs)
{
  if (control_mode == MODE_ABC ) {
    coordinates tmpfs;
    std::cerr << "set_foot_steps" << std::endl;

    gg->clear_footstep_node_list();
    for (size_t i = 0; i < fs.length(); i++) {
      std::string leg(fs[i].leg);
      if (leg == ":rleg" || leg == ":lleg") {
        memcpy(tmpfs.pos.data(), fs[i].pos, sizeof(double)*3);
        tmpfs.rot = (Eigen::Quaternion<double>(fs[i].rot[0], fs[i].rot[1], fs[i].rot[2], fs[i].rot[3])).normalized().toRotationMatrix(); // rtc: (x, y, z, w) but eigen: (w, x, y, z)
        std::cerr << leg; tmpfs.print_eus_coordinates(std::cerr);
        gg->append_footstep_node(leg, tmpfs);
      } else {
        std::cerr << "no such target : " << leg << std::endl;
        return false;
      }
    }
    gg->print_footstep_list();
    startWalking();
  }
 return true;
}

void AutoBalancer::waitFootSteps()
{
  //while (gg_is_walking) usleep(10);
  while (gg_is_walking && gg_solved) usleep(10);
  usleep(10);
  gg->set_offset_velocity_param(0,0,0);
}


//
extern "C"
{

    void AutoBalancerInit(RTC::Manager* manager)
    {
        RTC::Properties profile(autobalancer_spec);
        manager->registerFactory(profile,
                                 RTC::Create<AutoBalancer>,
                                 RTC::Delete<AutoBalancer>);
    }

};

