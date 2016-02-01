/*
 * Copyright (C) 2012-2015 Open Source Robotics Foundation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
*/

#include "gazebo/physics/physics.hh"
#include "gazebo/transport/transport.hh"
#include "sdcCar.hh"
#include <vector>
#include <chrono>

using namespace gazebo;
GZ_REGISTER_MODEL_PLUGIN(sdcCar)

const double DIRECTION_MARGIN_OF_ERROR = 0.00855;
const double STEERING_MARGIN_OF_ERROR = 0.05;
const int LIDAR_DETECTION_MARGIN_OF_ERROR = 2;

const double STEERING_ADJUSTMENT_RATE = 0.02;

const double PI = 3.14159265359;

// How much we can turn the "steering wheel"
const double STEERING_RANGE = 5 * PI;


/////////////////////////////////////////////////
sdcCar::sdcCar()
{

    this->joints.resize(4);

    this->aeroLoad = 0.1;
    this->swayForce = 10;

    this->maxSpeed = 10;
    this->frontPower = 50;
    this->rearPower = 50;
    this->wheelRadius = 0.3;
    this->maxBrake = 0.0;
    this->maxGas = 0.0;
    this->steeringRatio = 1.0;
    this->tireAngleRange = 1.0;

    this->gas = 0.0;
    this->brake = 0.0;

    this->maxCarSpeed = 6;
    this->maxCarReverseSpeed = -10;

    this->steeringAmount = 0.0;
    this->targetSteeringAmount = 0.0;
    this->targetDirection = Angle(0.0);
    this->turning = false;

    this->targetSpeed = 6;

    // Used to track waypoint driving
    this->waypointProgress = 0;

    this->atIntersection = 0;

    // Used to estimate speed of followed object
    this->estimatedSpeed = 0.0;
    this->lastPosition = 0.0;
    this->currentPosition = 0.0;
    this->speedCounter = 0.0;
    this->startTime = std::chrono::high_resolution_clock::time_point();
    this->endTime = std::chrono::high_resolution_clock::time_point();
}


/////////////////////////////////////////////////
void sdcCar::Load(physics::ModelPtr _model, sdf::ElementPtr _sdf)
{
  this->model = _model;

  this->joints[0] = this->model->GetJoint(_sdf->Get<std::string>("front_left"));
  this->joints[1] = this->model->GetJoint(_sdf->Get<std::string>("front_right"));
  this->joints[2] = this->model->GetJoint(_sdf->Get<std::string>("back_left"));
  this->joints[3] = this->model->GetJoint(_sdf->Get<std::string>("back_right"));

  this->joints[0]->SetParam("suspension_erp", 0, 0.15);
  this->joints[0]->SetParam("suspension_cfm", 0, 0.01);

  this->joints[1]->SetParam("suspension_erp", 0, 0.15);
  this->joints[1]->SetParam("suspension_cfm", 0, 0.01);

  this->joints[2]->SetParam("suspension_erp", 0, 0.15);
  this->joints[2]->SetParam("suspension_cfm", 0, 0.01);

  this->joints[3]->SetParam("suspension_erp", 0, 0.15);
  this->joints[3]->SetParam("suspension_cfm", 0, 0.01);

  this->maxSpeed = _sdf->Get<double>("max_speed");
  this->aeroLoad = _sdf->Get<double>("aero_load");
  this->tireAngleRange = _sdf->Get<double>("tire_angle_range");
  this->frontPower = _sdf->Get<double>("front_power");
  this->rearPower = _sdf->Get<double>("rear_power");

  this->connections.push_back(event::Events::ConnectWorldUpdateBegin(
          boost::bind(&sdcCar::OnUpdate, this)));
}

/////////////////////////////////////////////////
void sdcCar::Init()
{
  this->chassis = this->joints[0]->GetParent();

  // This assumes that the largest dimension of the wheel is the diameter
  physics::EntityPtr parent = boost::dynamic_pointer_cast<physics::Entity>(
      this->joints[0]->GetChild());
  math::Box bb = parent->GetBoundingBox();
  this->wheelRadius = bb.GetSize().GetMax() * 0.5;

  // Compute the angle ratio between the steering wheel and the tires
  this->steeringRatio = STEERING_RANGE / this->tireAngleRange;

  printf("SteeringRatio[%f] MaxGas[%f]\n", this->steeringRatio, this->maxGas);
}

/////////////////////////////////////////////////
void sdcCar::OnUpdate()
{

    // Get the steering angle
    //  double steeringAngle = this->steeringJoint->GetAngle(0).Radian();

    // Get the current velocity of the car
    this->velocity = this->chassis->GetWorldLinearVel();
    this->x = this->chassis->GetWorldPose().pos.x;
    this->y = this->chassis->GetWorldPose().pos.y;

    //std::cout << this->GetSpeed() << "\n";

    math::Pose pose = this->chassis->GetWorldPose();
    this->yaw = pose.rot.GetYaw();

    this->frontLidarUpdate();
    this->Drive();



  // Compute the angle of the front wheels.
  double wheelAngle = this->steeringAmount / this->steeringRatio;

  // double idleSpeed = 0.5;

  // Compute the rotational velocity of the wheels
  double jointVel = (std::max(0.0, this->gas-this->brake) * this->maxSpeed) /
                    this->wheelRadius;

  // Set velocity and max force for each wheel
  this->joints[0]->SetVelocityLimit(1, -jointVel);
  this->joints[0]->SetForce(1, -(this->gas + this->brake) * this->frontPower);

  this->joints[1]->SetVelocityLimit(1, -jointVel);
  this->joints[1]->SetForce(1, -(this->gas + this->brake) * this->frontPower);

  this->joints[2]->SetVelocityLimit(1, -jointVel);
  this->joints[2]->SetForce(1, -(this->gas + this->brake) * this->rearPower);

  this->joints[3]->SetVelocityLimit(1, -jointVel);
  this->joints[3]->SetForce(1, -(this->gas + this->brake) * this->rearPower);

  // Set the front-left wheel angle
  this->joints[0]->SetLowStop(0, wheelAngle);
  this->joints[0]->SetHighStop(0, wheelAngle);
  this->joints[0]->SetLowStop(0, wheelAngle);
  this->joints[0]->SetHighStop(0, wheelAngle);

  // Set the front-right wheel angle
  this->joints[1]->SetHighStop(0, wheelAngle);
  this->joints[1]->SetLowStop(0, wheelAngle);
  this->joints[1]->SetHighStop(0, wheelAngle);
  this->joints[1]->SetLowStop(0, wheelAngle);

  //  aerodynamics
  this->chassis->AddForce(
      math::Vector3(0, 0, this->aeroLoad * this->velocity.GetSquaredLength()));

  // Sway bars
  math::Vector3 bodyPoint;
  math::Vector3 hingePoint;
  math::Vector3 axis;

  for (int ix = 0; ix < 4; ++ix)
  {
    hingePoint = this->joints[ix]->GetAnchor(0);
    bodyPoint = this->joints[ix]->GetAnchor(1);

    axis = this->joints[ix]->GetGlobalAxis(0).Round();
    double displacement = (bodyPoint - hingePoint).Dot(axis);

    float amt = displacement * this->swayForce;
    if (displacement > 0)
    {
      if (amt > 15)
        amt = 15;

      math::Pose p = this->joints[ix]->GetChild()->GetWorldPose();
      this->joints[ix]->GetChild()->AddForce(axis * -amt);
      this->chassis->AddForceAtWorldPosition(axis * amt, p.pos);

      p = this->joints[ix^1]->GetChild()->GetWorldPose();
      this->joints[ix^1]->GetChild()->AddForce(axis * amt);
      this->chassis->AddForceAtWorldPosition(axis * -amt, p.pos);
    }
  }
}

/////////////////////////////////////////////////
void sdcCar::OnVelMsg(ConstPosePtr &_msg)
{
    std::cout << "Hello?\t" << _msg << std::endl;
}

/*
 * Both Accel and Brake call ApplyMovementForce
 * Caps max velocity and accelerates the vehicle.
 */
void sdcCar::ApplyMovementForce(double amt){
    if(amt > 0){
        if(this->GetSpeed() > this->maxCarSpeed){
            amt = 0;
        }
        this->gas = std::min(5.0, amt);
        this->brake = 0.0;
    } else {
        this->gas = 0.0;
        if(this->IsMovingForwards()){
            this->brake = std::max(this->maxCarReverseSpeed, amt);
        } else {
            this->brake = 0.0;
        }
    }
}

bool sdcCar::IsMovingForwards(){
    Angle velAngle = GetDirection();
    Angle carAngle = Angle(this->yaw);
    return (carAngle - velAngle).isFrontFacing();
}

/*
 * Default: 0.5
 */
void sdcCar::Accel(double amt){
//    amt = 3;
    this->ApplyMovementForce(amt);
}

/*
 * Default: 1.0
 */
void sdcCar::Brake(double amt){
//    amt = 7;
    this->ApplyMovementForce(-amt);
}

void sdcCar::MatchTargetSpeed(){
  if(this->GetSpeed() < this->targetSpeed){
    this->gas = 1.0;
    this->brake = 0.0;
  }else if(this->GetSpeed() > this->targetSpeed){
    this->gas = 0.0;
    if(this->IsMovingForwards()){
        this->brake = -2.0;
    } else {
        this->brake = 0.0;
    }
  }
}

/*
 * Handles turning based on the value of targetDirection. Calculates both which direction
 * to turn and by how much, as well as turning the actual wheel
 */
void sdcCar::Steer(){
    Angle directionAngleChange = this->GetDirection() - this->targetDirection;
    // If the car needs to turn, set the target steering amount
    if (!directionAngleChange.withinMargin(DIRECTION_MARGIN_OF_ERROR)) {
        // Angle proposedSteeringAngle = Angle(7*pow(sin(std::abs(directionAngleChange.angle)/2)-1,3)+7);
        double proposedSteeringAmount = fmax(fmin(-7*tan(directionAngleChange.angle/-2), 7), -7)*2;
        this->SetTargetSteeringAmount(proposedSteeringAmount);
    }

    // Check if the car needs to steer, and apply a small turn in the corresponding direction
    if (!(std::abs(this->targetSteeringAmount - this->steeringAmount) < STEERING_MARGIN_OF_ERROR)) {
        this->turning = true;
        if (this->steeringAmount < this->targetSteeringAmount) {
            this->steeringAmount = this->steeringAmount + STEERING_ADJUSTMENT_RATE;
        }else{
            this->steeringAmount = this->steeringAmount - STEERING_ADJUSTMENT_RATE;
        }
    } else {
        this->turning = false;
    }
}

// PRELIMINARY THOUGHTS ON A SMARTER TURNING ALGORITHM

// void sdcCar::SteerToPosition(double steeringRadius, Angle targetDirection){
//     Angle directionAngleChange = this->GetDirection() - this->targetDirection;
//     if(!directionAngleChange.withinMargin(DIRECTION_MARGIN_OF_ERROR)){
//       // 1.67 is the distance between wheels in the sdf
//       double proposedSteeringAmount = asin(1.67/steeringRadius);
//       this->SetTargetSteeringAmount(proposedSteeringAmount);
//     }
//
//     // Check if the car needs to steer, and apply a small turn in the corresponding direction
//     if (!(std::abs(this->targetSteeringAmount - this->steeringAmount) < STEERING_MARGIN_OF_ERROR)) {
//         this->turning = true;
//         if (this->steeringAmount < this->targetSteeringAmount) {
//             this->steeringAmount = this->steeringAmount + STEERING_ADJUSTMENT_RATE;
//         }else{
//             this->steeringAmount = this->steeringAmount - STEERING_ADJUSTMENT_RATE;
//         }
//     } else {
//         this->turning = false;
//     }
// }

/*
 * Sets a target direction for the car
 */
void sdcCar::SetTargetDirection(Angle direction){
    this->targetDirection = direction;
}

/*
 * Sets a target steering amount for the steering wheel
 */
void sdcCar::SetTargetSteeringAmount(double a){
    this->targetSteeringAmount = a;
}

void sdcCar::SetTargetSpeed(double s){
  // Caps the target speed, currently can't reverse
  this->targetSpeed = fmax(fmin(s, this->maxCarSpeed), 0);
}

/*
 * Gets the spped of the car
 */
double sdcCar::GetSpeed(){
    return sqrt(pow(this->velocity.x,2) + pow(this->velocity.y,2));
}

/*
 * Gets the current direction of the car
 */
Angle sdcCar::GetDirection(){
    math::Vector3 velocity = this->velocity;
    return Angle(atan2(velocity.y, velocity.x));
}
    //Updates Top LIDAR data
void sdcCar::topLidarUpdate(){
    this->tl = sdcSensorData::GetTopLidarRays();
    sdcCar::topForwardLidarUpdate(this->tl);
}
void sdcCar::topForwardLidarUpdate(std::vector<double> rays){
    this->tlForwardViews.clear();
    this->tlForwardRayLengths = 0;
    this->tlForwardSideRight = 404;
    this->tlForwardCenterRight = 404;
    this->tlForwardCenterLeft = -404;
    this->tlForwardSideLeft = -404;
    this->tlForwardNumRays = this->tl.size();
    this->tlForwardWeight = 0;

}
//     this->tlRightViews.clear();
//     this->tlRightRayLengths = 0;
//     this->tlRightSideRight = 404;
//     this->tlRightCenterRight = 404;
//     this->tlRightCenterLeft = -404;
//     this->tlRightSideLeft = -404;
//     this->tlRightNumRays = this->tl.size();
//     this->tlRightWeight = 0;
//
//     this->tlBackwardViews.clear();
//     this->tlBackwardRayLengths = 0;
//     this->tlBackwardSideRight = 404;
//     this->tlBackwardCenterRight = 404;
//     this->tlBackwardCenterLeft = -404;
//     this->tlBackwardSideLeft = -404;
//     this->tlBackwardNumRays = this->tl.size();
//     this->tlBackwardWeight = 0;
//
//     this->tlLeftViews.clear();
//     this->tlLeftRayLengths = 0;
//     this->tlLeftSideRight = 404;
//     this->tlLeftCenterRight = 404;
//     this->tlLeftCenterLeft = -404;
//     this->tlLeftSideLeft = -404;
//     this->tlLeftNumRays = this->tl.size();
//     this->tlLeftWeight = 0;
//     std::vector<int> leftView;
//     std::vector<int> rightView;
// }
  //Updates Front LIDAR data
void sdcCar::frontLidarUpdate(){
  this->flViews.clear();
  this->fl = sdcSensorData::GetFrontLidarRays();
  this->flRayLengths = 0;
  this->flSideRight = 404;
  this->flCenterRight = 404;
  this->flCenterLeft = -404;
  this->flSideLeft = -404;
  this->flNumRays = this->fl.size();
  this->flWeight = 0;
  std::vector<int> leftView;
  std::vector<int> rightView;

  //With this loop we track the areas of view the car has.
  for (int i = 0; i < this->flNumRays; ++i) {
      if(!std::isinf(this->fl[i])){
          if(i>319){
              --this->flWeight;
              this->flRayLengths += this->fl[i];
              leftView.push_back(i);
              if(this->flCenterLeft <  std::abs(i-639))
                  this->flCenterLeft = std::abs(i-639);
              this->flSideLeft = std::abs(i-639);

          } else {
              ++this->flWeight;
              this->flRayLengths += this->fl[i];
              rightView.push_back(i);
              this->flCenterRight = i;
              if(this->flSideRight > i)
                  this->flSideRight = i;
          }
      } else {
        if (leftView.size()!=0){
          this->flViews.push_back(leftView);
          leftView.clear();
        }
        if(rightView.size()!=0){
          this->flViews.push_back(rightView);
          rightView.clear();
        }
      }
  }
  if (leftView.size()!=0){
    this->flViews.push_back(leftView);
    leftView.clear();
  }
  if(rightView.size()!=0){
    this->flViews.push_back(rightView);
    rightView.clear();
  }
}

////////////////////////////////
////////////////////////////////
// BEGIN THE BRAIN OF THE CAR //
////////////////////////////////
////////////////////////////////

/*
 * Handles all logic for driving
 */
void sdcCar::Drive()
{
//    this->TurnRightIfObjectAhead();
//    this->DriveStraightThenStop();
//    this->DriveToCoordinates(0.0005, 0.0005);

    // Combines WalledDriving with WaypointDriving;
    // if (!(sdcSensorData::IsAllInf())) {
    //     this->WalledDriving();
    // } else {
    //     std::vector<math::Vector2d> waypoints = {math::Vector2d(0.0005,0.000), math::Vector2d(-0.0005,0.000), math::Vector2d(-0.001,-0.001)};
    //     this->WaypointDriving(waypoints);
    // }


    // this->DriveStraightThenStop();
    // this->WalledDriving();
    this->DetectIntersection();

    // this->Follow();
    // Handles all turning
    this->Steer();
    this->MatchTargetSpeed();

    //if(!std::isinf(sdcSensorData::GetCurrentCoord().x)) {
    //  double temp = (this->avg*this->count)/(this->count+1);
    //  double data = this->chassis->GetWorldPose().pos.x / sdcSensorData::GetCurrentCoord().x;
    //  this->avg = temp + data/(this->count+1);
    //  this->count += 1;

    //}
}

enum WaypointType {
  // Waypoint to visit, lowest priority
  WaypointType_Target,
  // Waypoints created programmatically, should be given higher priority
  WaypointType_DriveStraight,
  WaypointType_TurnLeft,
  WaypointType_TurnRight,
  WaypointType_Stop
};

class Waypoint {
  int waypointType;
  math::Vector2d pos;

  Waypoint(int waypointType, math::Vector2d pos) {
    this->waypointType = WaypointType_DriveStraight;
    this->pos = pos;
  }
};

/*
 * Drive from point to point in the given list
 */
void sdcCar::WaypointDriving(std::vector<math::Vector2d> waypoints){
    int progress = this->waypointProgress;
    //std::vector<math::Vector2d> waypoints = waypoints;
    //std::cout << progress << " / " << waypoints.size() << " " << (progress < waypoints.size()) << std::endl;
    if(progress < waypoints.size()){
        math::Vector2d nextTarget = waypoints[progress];
        Angle targetAngle = AngleToTarget(nextTarget);
        this->SetTargetDirection(targetAngle);

        //std::cout << targetAngle << std::endl;

        this->Accel();

        double distance = sdcSensorData::GetCurrentCoord().Distance(nextTarget);
        //std::cout << distance << std::endl;
        if (distance < 0.0001) {
            //std::cout << "#################################/nTarget achieved#################################/n";
            ++progress;
        }
    } else {
        this->Brake();
    }
    this->waypointProgress = progress;
}

// Returns the angle from the car's current position to a target position
Angle sdcCar::AngleToTarget(math::Vector2d target) {
    math::Vector2d position = sdcSensorData::GetCurrentCoord();
    math::Vector2d targetVector = math::Vector2d(target.x - position.x, target.y - position.y);
    return Angle(atan2(targetVector.y, targetVector.x));
}

// Drive with walled roads
//LIDAR 0-319 is right, 320-619 is left.
void sdcCar::WalledDriving(){
    //When driving down our current grid and the car stabilizes, centerRight and centerLeft are between 260-262 and drops down to 209 on intersections.
    if(this->atIntersection==0){
        this->SetTargetDirection(this->GetDirection() + Angle(this->flWeight*PI/320));
    }
}

// Drive in a straight line until it passes LON: 0.000200
void sdcCar::DriveStraightThenStop(){
     if (this->x > 200) {
         this->Brake();
     } else {
         this->Accel();
     }
}

void sdcCar::DriveStraightThenTurn(){
    Angle direction = this->GetDirection();
    this->Accel();
    if (this->x > 100) {
        this->SetTargetDirection(Angle(-PI/2));
    }
}
//Uses the front LIDAR sensor to detect an intersection.
//Based off of how many fields of view we have an what we can see we try to turn.
//When we are almost at an intersection slow down.
void sdcCar::DetectIntersection(){
  if(this->atIntersection == 0 && this->flViews.size() == 4 && this->flSideRight == 0 && this->flSideLeft == 0){
    this->SetTargetSpeed(2);
    this->atIntersection = 1;
  }else if (this->atIntersection == 1 && this->flViews.size() > 1 && this->flViews.size() < 4 && (this->flSideLeft != 0 && this->flSideRight != 0)){
      this->atIntersection = 2;
      this->SetTargetDirection(this->GetDirection() + PI/2);
  } else if (this->atIntersection == 2 && (this->GetDirection() - this->targetDirection).withinMargin(PI/8)){
      this->SetTargetSpeed(6);
      this->atIntersection = 0;
  }
}


// Car follows an object directly in front of it and slows down to stop when it starts to get close
void sdcCar::Follow() {
  using namespace std::chrono;
  std::vector<std::pair<int,int>> objectsInView;
  int lastIndex = -1;

  for(int i = 0; i < this->flNumRays; i++) {
    if(!std::isinf(this->fl[i])){
      if (lastIndex < 0) {
        lastIndex = i;
      }
    } else {
      if (lastIndex > 0) {
        objectsInView.push_back(std::make_pair(lastIndex, i-1));
        lastIndex = -1;
      }
    }
    if(lastIndex > 0){
      objectsInView.push_back(std::make_pair(lastIndex, this->flNumRays-1));
    }
  }
  //std::cout << objectsInView[0].first << "   " << objectsInView[0].second << std::endl;
  //std::cout << typeid(objectsInView[0].first).name() << std::endl;

  high_resolution_clock::time_point t1 = high_resolution_clock::now();
  startTime = t1;

  // system_clock::time_point sysTime = system_clock::now();
  // time_t tt;
  // tt = system_clock::to_time_t ( sysTime );
  // std::cout << "System time: " << ctime(&tt);


  if(this->flNumRays == 0) return;
  this->SetTargetSpeed(5);
  std::vector<double> closestPoint;
  // for(int i = 315; i < 326; i++){
  //   speedCounter++;
  //   closestPoint.push_back(fl[i]);
  //   //std::cout << closestPoint[0] << std::endl;
  //   if (!std::isinf(this->fl[i]) && (this->fl[i] <= 10)) {
  //     this->lastPosition = closestPoint[0];
  //     this->SetTargetSpeed(1);
  //     break;
  //   }
  // }

  high_resolution_clock::time_point t2 = high_resolution_clock::now();
  // if(speedCounter >= 100000){
  //   endTime = t2;
  //   duration<double> time_span = duration_cast<duration<double>>(endTime - startTime);
  //   std::cout << "Time elapsed: " << time_span.count() << std::endl;
  // }

  for(int i = 315; i < 326; i++){
    if (!std::isinf(this->fl[i]) && (this->fl[i] <= 20) && (this->fl[i] > 10)) {
      lastPosition = this->fl[i];
      break;
    } if (!std::isinf(this->fl[i]) && (this->fl[i] <= 10)) {
        currentPosition = this->fl[i];
        endTime = t2;
        duration<double> time_span = duration_cast<duration<double>>(endTime - startTime);
        estimatedSpeed = (currentPosition - lastPosition) / (time_span.count() * 1000000);
        this->SetTargetSpeed(estimatedSpeed);
        break;
    }
  }


  // for (int i = 0; i < numrays; ++i) {
  //   if (i >= 315 && i <= 325 && std::isinf(lidar[i])) {
  //     this->SetTargetSpeed(6);
  //   } else if (!std::isinf(lidar[i]) && (lidar[i] <= 10)) {
  //     this->SetTargetSpeed(0);
  //   }
  // }
}
