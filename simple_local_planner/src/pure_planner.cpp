/*********************************************************************
*
* Author: Fernando Caballero Benítez
* Author: Noé Pérez Higueras
*********************************************************************/

#include <simple_local_planner/pure_planner.h>
#include <costmap_2d/footprint.h>
#include <string>
#include <sstream>
#include <math.h>
#include <angles/angles.h>



#include <boost/algorithm/string.hpp>

#include <ros/console.h>

//for computing path distance
#include <queue>

using namespace std;
using namespace costmap_2d;

namespace simple_local_planner{

  void PurePlanner::reconfigure(SimpleLocalPlannerConfig &cfg)
  {
      SimpleLocalPlannerConfig config(cfg);

      boost::mutex::scoped_lock l(configuration_mutex_);
      
		acc_lim_trans_ = config.max_trans_acc;
		acc_lim_rot_ = config.max_rot_acc;
		max_vel_x_ = config.max_trans_vel;
		min_vel_x_ = config.min_trans_vel;
		max_vel_th_ = config.max_rot_vel;
		min_vel_th_ = config.min_rot_vel;
		min_in_place_vel_th_ = config.min_in_place_rot_vel;
		goal_lin_tolerance_ = config.xy_goal_tolerance;
		goal_ang_tolerance_ = config.yaw_goal_tolerance;
		wp_tolerance_ = config.wp_tolerance; 
		sim_time_ = config.sim_time;
		sim_granularity_ = config.sim_granularity;
		angular_sim_granularity_ = config.angular_sim_granularity;


  }



  PurePlanner::PurePlanner(WorldModel& world_model, 
          	const costmap_2d::Costmap2D& costmap,
			std::vector<geometry_msgs::Point> footprint_spec,
			double controller_freq,
			double max_trans_vel, double min_trans_vel,
			double max_rot_vel, double min_rot_vel,
			double min_in_place_rot_vel,
			double max_trans_acc, double max_rot_acc,
			double yaw_goal_tolerance, double xy_goal_tolerance,
			double wp_tolerance, double sim_time,
			double sim_granularity, double angular_sim_granularity)
				: world_model_(world_model), costmap_(costmap), footprint_spec_(footprint_spec)
  {

		costmap_2d::calculateMinAndMaxDistances(footprint_spec_, inscribed_radius_, circumscribed_radius_);
    
		controller_freq_ = controller_freq;
		goal_reached_ = false;

    	//For pure-pursuit
    	running_ = false;
		new_plan_ = false;
		wp_index_ = -1;	
    
    	acc_lim_trans_ = max_trans_acc;
		acc_lim_rot_ = max_rot_acc;
		max_vel_x_ = max_trans_vel;
		min_vel_x_ = min_trans_vel;
		max_vel_th_ = max_rot_vel;
		min_vel_th_ = min_rot_vel;
		min_in_place_vel_th_ = min_in_place_rot_vel;
		goal_lin_tolerance_ = xy_goal_tolerance;
		goal_ang_tolerance_ = yaw_goal_tolerance;
		wp_tolerance_ = wp_tolerance; 
		sim_time_ = sim_time;
		sim_granularity_ = sim_granularity;
		angular_sim_granularity_ = angular_sim_granularity;	

  }




  PurePlanner::~PurePlanner(){}




  /**
   * create and score a trajectory given the current pose of the robot and selected velocities
   */
  void PurePlanner::generateTrajectory(
      double x, double y, double theta, double vx, double vy,
          double vtheta, double vx_samp, double vy_samp, double vtheta_samp, double acc_x,
          double acc_y, double acc_theta, Trajectory& traj) {

    // make sure the configuration doesn't change mid run
    //boost::mutex::scoped_lock l(configuration_mutex_);

    double x_i = x;
    double y_i = y;
    double theta_i = theta;

    double vx_i , vy_i, vtheta_i;

    vx_i = vx;
    vy_i = vy;
	vtheta_i = vtheta;

    //compute the magnitude of the velocities
    double vmag = hypot(vx_samp, vy_samp);

    //compute the number of steps we must take along this trajectory to be "safe"
    int num_steps;

      num_steps = int(max((vmag * sim_time_) / sim_granularity_, fabs(vtheta_samp) / angular_sim_granularity_) + 0.5);
    //} else {
    //  num_steps = int(sim_time_ / sim_granularity_ + 0.5);
    //}

    //we at least want to take one step... even if we won't move, we want to score our current position
    if(num_steps == 0) {
      num_steps = 1;
    }

    double dt = sim_time_ / num_steps;
    double time = 0.0;

    //create a potential trajectory
    traj.resetPoints();
    traj.xv_ = vx_samp;
    traj.yv_ = vy_samp;
    traj.thetav_ = vtheta_samp;
    traj.cost_ = -1.0;


    for(int i = 0; i < num_steps; ++i) 
	{
      //get map coordinates of a point
      unsigned int cell_x, cell_y;

      //we don't want a path that goes off the know map
      if(!costmap_.worldToMap(x_i, y_i, cell_x, cell_y)){
        traj.cost_ = -1.0;
        return;
      }

      //check the point on the trajectory for legality
      double footprint_cost = footprintCost(x_i, y_i, theta_i);

      //if the footprint hits an obstacle this trajectory is invalid
      if(footprint_cost < 0){
        traj.cost_ = -1.0;
        return;
      }

      //the point is legal... add it to the trajectory
      traj.addPoint(x_i, y_i, theta_i);

      //calculate velocities
      vx_i = computeNewVelocity(vx_samp, vx_i, acc_x, dt);
      vy_i = computeNewVelocity(vy_samp, vy_i, acc_y, dt);
      vtheta_i = computeNewVelocity(vtheta_samp, vtheta_i, acc_theta, dt);

      //calculate positions
      x_i = computeNewXPosition(x_i, vx_i, vy_i, theta_i, dt);
      y_i = computeNewYPosition(y_i, vx_i, vy_i, theta_i, dt);
      theta_i = computeNewThetaPosition(theta_i, vtheta_i, dt);

      //increment time
      time += dt;
    } // end for i < numsteps

    traj.cost_ = 0;

  }




  /*double PurePlanner::headingDiff(int cell_x, int cell_y, double x, double y, double heading){
    double heading_diff = DBL_MAX;
    unsigned int goal_cell_x, goal_cell_y;
    const double v2_x = cos(heading);
    const double v2_y = sin(heading);

    //find a clear line of sight from the robot's cell to a point on the path
    for (int i = global_plan_.size() - 1; i >=0; --i) {
      if (costmap_.worldToMap(global_plan_[i].pose.position.x, global_plan_[i].pose.position.y, goal_cell_x, goal_cell_y)) {
        if (lineCost(cell_x, goal_cell_x, cell_y, goal_cell_y) >= 0) {
          double gx, gy;
          costmap_.mapToWorld(goal_cell_x, goal_cell_y, gx, gy);
          double v1_x = gx - x;
          double v1_y = gy - y;

          double perp_dot = v1_x * v2_y - v1_y * v2_x;
          double dot = v1_x * v2_x + v1_y * v2_y;

          //get the signed angle
          double vector_angle = atan2(perp_dot, dot);

          heading_diff = fabs(vector_angle);
          return heading_diff;
        }
      }
    }
    return heading_diff;
  }*/





  //calculate the cost of a ray-traced line
  double PurePlanner::lineCost(int x0, int x1,
      int y0, int y1){
    //Bresenham Ray-Tracing
    int deltax = abs(x1 - x0);        // The difference between the x's
    int deltay = abs(y1 - y0);        // The difference between the y's
    int x = x0;                       // Start x off at the first pixel
    int y = y0;                       // Start y off at the first pixel

    int xinc1, xinc2, yinc1, yinc2;
    int den, num, numadd, numpixels;

    double line_cost = 0.0;
    double point_cost = -1.0;

    if (x1 >= x0)                 // The x-values are increasing
    {
      xinc1 = 1;
      xinc2 = 1;
    }
    else                          // The x-values are decreasing
    {
      xinc1 = -1;
      xinc2 = -1;
    }

    if (y1 >= y0)                 // The y-values are increasing
    {
      yinc1 = 1;
      yinc2 = 1;
    }
    else                          // The y-values are decreasing
    {
      yinc1 = -1;
      yinc2 = -1;
    }

    if (deltax >= deltay)         // There is at least one x-value for every y-value
    {
      xinc1 = 0;                  // Don't change the x when numerator >= denominator
      yinc2 = 0;                  // Don't change the y for every iteration
      den = deltax;
      num = deltax / 2;
      numadd = deltay;
      numpixels = deltax;         // There are more x-values than y-values
    } else {                      // There is at least one y-value for every x-value
      xinc2 = 0;                  // Don't change the x for every iteration
      yinc1 = 0;                  // Don't change the y when numerator >= denominator
      den = deltay;
      num = deltay / 2;
      numadd = deltax;
      numpixels = deltay;         // There are more y-values than x-values
    }

    for (int curpixel = 0; curpixel <= numpixels; curpixel++) {
      point_cost = pointCost(x, y); //Score the current point

      if (point_cost < 0) {
        return -1;
      }

      if (line_cost < point_cost) {
        line_cost = point_cost;
      }

      num += numadd;              // Increase the numerator by the top of the fraction
      if (num >= den) {           // Check if numerator >= denominator
        num -= den;               // Calculate the new numerator value
        x += xinc1;               // Change the x as appropriate
        y += yinc1;               // Change the y as appropriate
      }
      x += xinc2;                 // Change the x as appropriate
      y += yinc2;                 // Change the y as appropriate
    }

    return line_cost;
  }

  double PurePlanner::pointCost(int x, int y){
    unsigned char cost = costmap_.getCost(x, y);
    //if the cell is in an obstacle the path is invalid
    if(cost == LETHAL_OBSTACLE || cost == INSCRIBED_INFLATED_OBSTACLE || cost == NO_INFORMATION){
      return -1;
    }

    return cost;
  }



  bool PurePlanner::updatePlan(const vector<geometry_msgs::PoseStamped>& new_plan)
  {
		goal_reached_ = false;

		// Copy new plan 
		global_plan_.clear();
		global_plan_.resize(new_plan.size());
		for(unsigned int i = 0; i < new_plan.size(); ++i)
		{
			global_plan_[i] = new_plan[i];
		}
		
		// Check plan size
		if(global_plan_.size() == 0)
		{
			running_ = false;
			wp_index_ = -1;
			ROS_WARN("New local plan size = 0!");
			return true;
		}
		
		// Set the way-point index to the first point of the path
		wp_index_ = 0;
		running_ = true;
		new_plan_ = true;
		
		// Set plan goal point
		geometry_msgs::PoseStamped& goal_pose = global_plan_[global_plan_.size()-1];
		goal_x_ = goal_pose.pose.position.x;
		goal_y_ = goal_pose.pose.position.y;
		goal_t_ = tf::getYaw(goal_pose.pose.orientation);
		
		// Set the plan starting point
		geometry_msgs::PoseStamped& start_pose = global_plan_[0];
		start_x_ = start_pose.pose.position.x;
		start_y_ = start_pose.pose.position.y;
		start_t_ = tf::getYaw(start_pose.pose.orientation);
		
		/*if(compute_dists) 
		{
			//reset the map for new operations
			path_map_.resetPathDist();
			goal_map_.resetPathDist();

			//make sure that we update our path based on the global plan and compute costs
			path_map_.setTargetCells(costmap_, global_plan_);
			goal_map_.setLocalGoal(costmap_, global_plan_);
			ROS_DEBUG("Path/Goal distance computed");
		}*/
		return true;
  }


  bool PurePlanner::checkTrajectory(double x, double y, double theta, double vx, double vy,
      double vtheta, double vx_samp, double vy_samp, double vtheta_samp){
    
	Trajectory t;

	generateTrajectory(x, y, theta,
                       vx, vy, vtheta,
                       vx_samp, vy_samp, vtheta_samp,
                       acc_lim_trans_, 0.0, acc_lim_rot_, t);

	if(t.cost_ < 0.0) {
		ROS_WARN("Invalid Trajectory vx:%f, vy:%f, vth:%f, cost: %f", vx, vy, vtheta, t.cost_);
		return false;
    }
    if(isnan(t.cost_)) {
		ROS_WARN("Trajectory cost is not a number!!! Invalid Trajectory vx:%f, vy:%f, vth:%f, cost: %f", vx, vy, vtheta, t.cost_);
		//cost = 0.0;
		return false;
	}
    
    //otherwise the trajectory is valid
    return true;
  }







  bool PurePlanner::isGoalReached()
  {
		if(goal_reached_) {
			goal_reached_ = false; //we reset the flag
			return true;
		}
		return goal_reached_;
  }

  void PurePlanner::resetGoal() {
		goal_reached_ = false;
  }


  //given the current state of the robot, find a good control command
  bool PurePlanner::findBestAction(tf::Stamped<tf::Pose> global_pose, tf::Stamped<tf::Pose> global_vel,
      geometry_msgs::Twist& cmd_vel)
  {

	boost::mutex::scoped_lock l(configuration_mutex_);
	
	goal_reached_ = false;
	double vx, vy = 0.0, vt;
	
	// Check we have a path and we are running
	if(!running_)
	{
		vx = 0.0;
		vt = 0.0;
		cmd_vel.linear.x = vx;
		cmd_vel.linear.y = 0.0;
		cmd_vel.linear.z = 0.0;
		cmd_vel.angular.x = 0.0;
		cmd_vel.angular.y = 0.0;
		cmd_vel.angular.z = vt;
		//printf("PurePursuit waiting for a path\n");
		return true;
	}
		
	// Get current robot position and velocity in X, Y and Theta
	float rx, ry, rt, rvx, rvy, rvt;
	rx = global_pose.getOrigin().getX();
	ry = global_pose.getOrigin().getY();
	rt = tf::getYaw(global_pose.getRotation());
	rvx = global_vel.getOrigin().getX();
	rvy = global_vel.getOrigin().getY();
	rvt = tf::getYaw(global_vel.getRotation());
	
    //reset the map for new operations
    /*path_map_.resetPathDist();
    goal_map_.resetPathDist();

    //mark cells within the initial footprint of the robot
    Eigen::Vector3f pos(global_pose.getOrigin().getX(), global_pose.getOrigin().getY(), tf::getYaw(global_pose.getRotation()));
    std::vector<upo_navigation::Position2DInt> footprint_list =
        footprint_helper_.getFootprintCells(
            pos,
            footprint_spec_,
            costmap_,
            true);
    for (unsigned int i = 0; i < footprint_list.size(); ++i) {
      path_map_(footprint_list[i].x, footprint_list[i].y).within_robot = true;
    }*/

    //make sure that we update our path based on the global plan and compute costs
    //path_map_.setTargetCells(costmap_, global_plan_);
    //goal_map_.setLocalGoal(costmap_, global_plan_);
    //ROS_DEBUG("Path/Goal distance computed");

	// Check if we are close enought to the goal
	double dist_goal = sqrt((rx-goal_x_)*(rx-goal_x_)+(ry-goal_y_)*(ry-goal_y_));
	double dist_start = sqrt((rx-start_x_)*(rx-start_x_)+(ry-start_y_)*(ry-start_y_)); 

	//printf("in_place: %.2f, max_th: %.2f, min_th: %.2f\n", min_in_place_vel_th_, max_vel_th_, min_vel_th_);
	if(dist_goal < goal_lin_tolerance_)
	{
		// Stop the robot
		vx = 0.0;
		vy = 0.0;
		
		// Rotate at minumin velocity until reaching the goal angle
		if(fabs(goal_t_-rt) < goal_ang_tolerance_)
		{
			vt = 0.0;
			running_ = false;
			goal_reached_ = true; 
		}
		else if(goal_t_ > rt)
			vt = min_in_place_vel_th_;
		else
			vt = -min_in_place_vel_th_;
		
		// Modified by Noé
		/*else {
			double ang_diff = angles::shortest_angular_distance(rt, goal_t_);
			//printf("Angle diff: %.3f\n", ang_diff*180.0/M_PI);
			if(ang_diff > 0.0)
				vt = min_in_place_vel_th_;
			else
				vt = -min_in_place_vel_th_;
		}*/
		//Added by Noé
		cmd_vel.linear.x = vx;
		cmd_vel.linear.y = vy;
		cmd_vel.linear.z = 0.0;
		cmd_vel.angular.x = 0.0;
		cmd_vel.angular.y = 0.0;
		cmd_vel.angular.z = vt;
		return true;
		
	}

	// Do we have a new plan? get the closest point into the plan
	if(new_plan_)
	{
		new_plan_ = false;
		double dist;
		wp_index_ = 0;
		for(int i=global_plan_.size()-1; i>=0; i--)
		{
			double wpx = global_plan_[i].pose.position.x;
			double wpy = global_plan_[i].pose.position.y;
			dist = sqrt((rx-wpx)*(rx-wpx)+(ry-wpy)*(ry-wpy));
			if(dist < wp_tolerance_)
			{
				wp_index_ = i;
				break;
			}
		}
	}
	
	// Get current way-point in the path
	double wpx = global_plan_[wp_index_].pose.position.x;
	double wpy = global_plan_[wp_index_].pose.position.y;
	
	// Is this way-point still valid?
	double dist_swp = sqrt((rx-wpx)*(rx-wpx)+(ry-wpy)*(ry-wpy)); 
	while(dist_swp < wp_tolerance_ && wp_index_ < (int)global_plan_.size()-1)
	{
		wp_index_++;
		wpx = global_plan_[wp_index_].pose.position.x;
		wpy = global_plan_[wp_index_].pose.position.y;
		dist_swp = sqrt((rx-wpx)*(rx-wpx)+(ry-wpy)*(ry-wpy));
	}

	// Transform way-point into local robot frame and get desired x,y,theta
	double dx = (wpx-rx)*cos(rt) + (wpy-ry)*sin(rt);
	double dy =-(wpx-rx)*sin(rt) + (wpy-ry)*cos(rt);
	double dt = atan2(dy, dx);
	

	double incr = 1/controller_freq_;

	// Check if we need rotation in place before moving the robot to reach the way-point
	if(fabs(dt) > 0.75) //0.7~41º  0.79~45º
	{
		vx = rvx-incr;  
		if(vx < 0.0)
			vx = 0.0;
		vy = 0.0;
		vt = min_in_place_vel_th_;
		if(dt < 0.0)
			vt = -min_in_place_vel_th_;
			
		//printf("place_vt: %.2f\n", vt);
	}
	else // Select the linear and angular velocities to reach the way-point
	{
		// Compute actions depending to the distance to the goal and the starting points
		double dist_th = 1.5;
		if(dist_goal < dist_th)
		{
			vx = min_vel_x_ + (max_vel_x_ - min_vel_x_)*dist_goal/dist_th;
			vy = 0.0;
			vt = min_vel_th_ + (max_vel_th_ - min_vel_th_)*dist_goal/dist_th;
			//--added by noe
			if(dt < 0.0)
				vt *= -1;
		}
		/*else if(dist_start < dist_th)
		{
			vx = min_vel_x_ + (max_vel_x_ - min_vel_x_)*dist_start/dist_th;
			vy = 0.0;
			vt = min_vel_th_ + (max_vel_th_ - min_vel_th_)*dist_start/dist_th;
		}*/
		else
		{
			
			vx = max_vel_x_* exp(-fabs(dt)) * tanh(4*dist_swp); //max_vel_x_;
			vy = 0.0;
			vt = max_vel_th_* dt; //max_vel_th_;
		}

		//if(vx > rvx+incr)
		//	vx = rvx+incr;
		
		// Set the sign of the commanded angular velocity and reset in case of small variations
		//if(dt < 0.0) //commented by Noé
		//	vt *= -1;
		if(fabs(dt) < 0.1)
			vt = 0.0;
	}
	
	// Check if the action collide with an obstacle
	if(checkTrajectory(rx, ry, rt, vx, vy, vt, 1.0, 0.0, 1.0) || fabs(vx) < 0.0001)	
	{
		cmd_vel.linear.x = vx;
		cmd_vel.linear.y = vy;
		cmd_vel.linear.z = 0.0;
		cmd_vel.angular.x = 0.0;
		cmd_vel.angular.y = 0.0;
		cmd_vel.angular.z = vt;
		//printf("max_vel_x: %.3f, vx: %.3f, vth: %.3f\n", max_vel_x_, vx, vt);
		return true;
	}
	//---previous code-----
	//else
	//{
		//// Stop the robot
		//cmd_vel.linear.x = 0.0;
		//cmd_vel.linear.y = 0.0;
		//cmd_vel.linear.z = 0.0;
		//cmd_vel.angular.x = 0.0;
		//cmd_vel.angular.y = 0.0;
		//cmd_vel.angular.z = 0.0;
		//return false;
	//}
	//---------------------
	//+++++++code added by Noé
	else //try to perform a rotation in place to reduce more the angle if we have obstacles
	{
		//option 1
		if(dt > 0.09 || dt < -0.09) //0.09rad~5º
		{
			vx = 0.0;
			vy = 0.0;
			vt = min_in_place_vel_th_;
			if(dt < 0.0)
				vt = -min_in_place_vel_th_;
				
			cmd_vel.linear.x = vx;
			cmd_vel.linear.y = vy;
			cmd_vel.linear.z = 0.0;
			cmd_vel.angular.x = 0.0;
			cmd_vel.angular.y = 0.0;
			cmd_vel.angular.z = vt;
			return true;
		} else {
			// Stop the robot
			cmd_vel.linear.x = 0.0;
			cmd_vel.linear.y = 0.0;
			cmd_vel.linear.z = 0.0;
			cmd_vel.angular.x = 0.0;
			cmd_vel.angular.y = 0.0;
			cmd_vel.angular.z = 0.0;
			return false;
		}
		
		//option 2
		/*vx = rvx-incr;
		if(vx < 0.0)
			vx = 0.0;
		vy = 0.0;
		vt = min_in_place_vel_th_;
		if(dt < 0.0)
			vt = -min_in_place_vel_th_;
			
		if(checkTrajectory(rx, ry, rt, vx, vy, vt, 1.0, 0.0, 1.0))	
		{
			cmd_vel.linear.x = vx;
			cmd_vel.linear.y = vy;
			cmd_vel.linear.z = 0.0;
			cmd_vel.angular.x = 0.0;
			cmd_vel.angular.y = 0.0;
			cmd_vel.angular.z = vt;
			return true;
		} else {
			// Stop the robot
			cmd_vel.linear.x = 0.0;
			cmd_vel.linear.y = 0.0;
			cmd_vel.linear.z = 0.0;
			cmd_vel.angular.x = 0.0;
			cmd_vel.angular.y = 0.0;
			cmd_vel.angular.z = 0.0;
			return false;
		}*/
		
	}
	//+++++++++++++++++++++++++
  }



  //we need to take the footprint of the robot into account when we calculate cost to obstacles
  double PurePlanner::footprintCost(double x_i, double y_i, double theta_i){
    //check if the footprint is legal
    return world_model_.footprintCost(x_i, y_i, theta_i, footprint_spec_, inscribed_radius_, circumscribed_radius_);
  }


  /*void TrajectoryPlannerPP::getLocalGoal(double& x, double& y){
    x = path_map_.goal_x_;
    y = path_map_.goal_y_;
  }*/

};
