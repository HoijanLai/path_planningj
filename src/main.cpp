#include <fstream>
#include <math.h>
#include <uWS/uWS.h>
#include <chrono>
#include <iostream>
#include <thread>
#include <vector>
#include "Eigen-3.3/Eigen/Core"
#include "Eigen-3.3/Eigen/QR"
#include "json.hpp"
#include "spline.h"

using namespace std;

// for convenience
using json = nlohmann::json;

// For converting back and forth between radians and degrees.
constexpr double pi() { return M_PI; }
double deg2rad(double x) { return x * pi() / 180; }
double rad2deg(double x) { return x * 180 / pi(); }

// Checks if the SocketIO event has JSON data.
// If there is data the JSON object in string format will be returned,
// else the empty string "" will be returned.
string hasData(string s) {
  auto found_null = s.find("null");
  auto b1 = s.find_first_of("[");
  auto b2 = s.find_first_of("}");
  if (found_null != string::npos) {
    return "";
  } else if (b1 != string::npos && b2 != string::npos) {
    return s.substr(b1, b2 - b1 + 2);
  }
  return "";
}

double distance(double x1, double y1, double x2, double y2)
{
  return sqrt((x2-x1)*(x2-x1)+(y2-y1)*(y2-y1));
}
int ClosestWaypoint(double x, double y, vector<double> maps_x, vector<double> maps_y)
{

  double closestLen = 100000; //large number
  int closestWaypoint = 0;

  for(int i = 0; i < maps_x.size(); i++)
  {
    double map_x = maps_x[i];
    double map_y = maps_y[i];
    double dist = distance(x,y,map_x,map_y);
    if(dist < closestLen)
    {
      closestLen = dist;
      closestWaypoint = i;
    }

  }

return closestWaypoint;

}

int NextWaypoint(double x, double y, double theta, vector<double> maps_x, vector<double> maps_y)
{

  int closestWaypoint = ClosestWaypoint(x,y,maps_x,maps_y);

  double map_x = maps_x[closestWaypoint];
  double map_y = maps_y[closestWaypoint];

  double heading = atan2( (map_y-y),(map_x-x) );

  double angle = abs(theta-heading);

  if(angle > pi()/4)
  {
    closestWaypoint++;
  }

  return closestWaypoint;

}

// Transform from Cartesian x,y coordinates to Frenet s,d coordinates
vector<double> getFrenet(double x, double y, double theta, vector<double> maps_x, vector<double> maps_y)
{
  int next_wp = NextWaypoint(x,y, theta, maps_x,maps_y);

  int prev_wp;
  prev_wp = next_wp-1;
  if(next_wp == 0)
  {
    prev_wp  = maps_x.size()-1;
  }

  double n_x = maps_x[next_wp]-maps_x[prev_wp];
  double n_y = maps_y[next_wp]-maps_y[prev_wp];
  double x_x = x - maps_x[prev_wp];
  double x_y = y - maps_y[prev_wp];

  // find the projection of x onto n
  double proj_norm = (x_x*n_x+x_y*n_y)/(n_x*n_x+n_y*n_y);
  double proj_x = proj_norm*n_x;
  double proj_y = proj_norm*n_y;

  double frenet_d = distance(x_x,x_y,proj_x,proj_y);

  //see if d value is positive or negative by comparing it to a center point

  double center_x = 1000-maps_x[prev_wp];
  double center_y = 2000-maps_y[prev_wp];
  double centerToPos = distance(center_x,center_y,x_x,x_y);
  double centerToRef = distance(center_x,center_y,proj_x,proj_y);

  if(centerToPos <= centerToRef)
  {
    frenet_d *= -1;
  }

  // calculate s value
  double frenet_s = 0;
  for(int i = 0; i < prev_wp; i++)
  {
    frenet_s += distance(maps_x[i],maps_y[i],maps_x[i+1],maps_y[i+1]);
  }

  frenet_s += distance(0,0,proj_x,proj_y);

  return {frenet_s,frenet_d};

}


// Transform from Frenet s,d coordinates to Cartesian x,y
vector<double> getXY(double s, double d, vector<double> maps_s, vector<double> maps_x, vector<double> maps_y)
{
  int prev_wp = -1;

  while(s > maps_s[prev_wp+1] && (prev_wp < (int)(maps_s.size()-1) ))
  {
    prev_wp++;
  }

  int wp2 = (prev_wp+1)%maps_x.size();

  double heading = atan2((maps_y[wp2]-maps_y[prev_wp]),(maps_x[wp2]-maps_x[prev_wp]));
  // the x,y,s along the segment
  double seg_s = (s-maps_s[prev_wp]);

  double seg_x = maps_x[prev_wp]+seg_s*cos(heading);
  double seg_y = maps_y[prev_wp]+seg_s*sin(heading);

  double perp_heading = heading-pi()/2;

  double x = seg_x + d*cos(perp_heading);
  double y = seg_y + d*sin(perp_heading);

  return {x,y};

}

int main() {
  uWS::Hub h;

  // Load up map values for waypoint's x,y,s and d normalized normal vectors
  vector<double> map_waypoints_x;
  vector<double> map_waypoints_y;
  vector<double> map_waypoints_s;
  vector<double> map_waypoints_dx;
  vector<double> map_waypoints_dy;

  // Waypoint map to read from
  string map_file_ = "../data/highway_map.csv";
  // The max s value before wrapping around the track back to 0
  double max_s = 6945.554;

  ifstream in_map_(map_file_.c_str(), ifstream::in);

  string line;
  while (getline(in_map_, line)) {
    istringstream iss(line);
    double x;
    double y;
    float s;
    float d_x;
    float d_y;
    iss >> x;
    iss >> y;
    iss >> s;
    iss >> d_x;
    iss >> d_y;
    map_waypoints_x.push_back(x);
    map_waypoints_y.push_back(y);
    map_waypoints_s.push_back(s);
    map_waypoints_dx.push_back(d_x);
    map_waypoints_dy.push_back(d_y);
  }

  int lane = 1;
  double target_vel = 49.5;
  double ref_vel = 0;

  enum state {KL, LCL, LCR, PLCL, PLCR}; 
  state car_state = KL;

  h.onMessage([&car_state,&target_vel,&ref_vel,&lane,&map_waypoints_x,&map_waypoints_y,&map_waypoints_s,&map_waypoints_dx,&map_waypoints_dy](uWS::WebSocket<uWS::SERVER> ws, char *data, size_t length,
                     uWS::OpCode opCode) {
    // "42" at the start of the message means there's a websocket message event.
    // The 4 signifies a websocket message
    // The 2 signifies a websocket event
    //auto sdata = string(data).substr(0, length);
    //cout << sdata << endl;
    if (length && length > 2 && data[0] == '4' && data[1] == '2') {

      auto s = hasData(data);

      if (s != "") {
        auto j = json::parse(s);
        
        string event = j[0].get<string>();
        
        if (event == "telemetry") {
          // j[1] is the data JSON object
          
          // Main car's localization Data
          double car_x = j[1]["x"];
          double car_y = j[1]["y"];
          double car_s = j[1]["s"];
          double car_d = j[1]["d"];
          double car_yaw = j[1]["yaw"];
          double car_speed = j[1]["speed"];

          // Previous path data given to the Planner
          auto previous_path_x = j[1]["previous_path_x"];
          auto previous_path_y = j[1]["previous_path_y"];

          // Previous path's end s and d values 
          double end_path_s = j[1]["end_path_s"];
          double end_path_d = j[1]["end_path_d"];
          double pre_size = previous_path_x.size();            

          cout << "****************************************" << endl; 
          cout << "============| " << "car location: " << car_x << ", " << car_y << endl;
          cout << "============| " << "car yaw     : " << car_yaw << endl;
          cout << "============| " << "car s       : " << car_s << endl; 
          cout << "============| " << "car velocity: " << car_speed << endl;

          double pred_s = car_s;
          if (pre_size > 0) pred_s = end_path_s; // assume the car has realize to the end of the previous plan

          // Sensor Fusion Data, a list of all other cars on the same side of the road.
          auto sensor_fusion = j[1]["sensor_fusion"];
          int idx_ID = 0, idx_X = 1, idx_Y = 2, idx_VX = 3, idx_VY = 4, idx_S = 5, idx_D = 6;

          cout << "<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<" << endl;
          cout << "number of detected cars : " << sensor_fusion.size() << endl;

          // ==========================================
          // drqw useful information form sensor fusion
          // ==========================================
          double safe_dist = 25.0; 
          bool too_close = false; // whether there's a car close enough ahead
          double s_ul =  99999, s_ur =  99999; // ul: upper left
          double s_ll = -99999, s_lr = -99999;
          double s_u  =  99999;
          double v_u = target_vel;
          cout << "--------------------------" << endl;
          cout << "sd of ego car : <" << pred_s << ", " << car_d << ">" << endl;
          for (int i = 0; i < sensor_fusion.size(); ++i) {
            double d = sensor_fusion[i][idx_D];
            double vx = sensor_fusion[i][idx_VX];
            double vy = sensor_fusion[i][idx_VY];
            double vn = sqrt(vx*vx + vy*vy);            
            double other_s = sensor_fusion[i][idx_S];
            double ahead_s = other_s + (double)pre_size*0.02*vn; // naive prediction of the car ahead
            double diff = ahead_s - pred_s; 
            cout << "sd of car #" << sensor_fusion[i][idx_ID] << " : <" << diff << ", " << d << ">" << endl;     

            // **[1]** set some flag for cars on the same lane
            if (d <= (4*lane+2)+2.5 && d >= (4*lane+2)-2.5 && diff > 0) {
              if (diff <= safe_dist) too_close = true;
              if (diff < s_u) {
                s_u = diff;
                v_u = vn; 
              }
            }
            // **[2]** right lane  
            else if (d > (4*lane+2)+2) {
              if      (diff > 0 && diff < s_ur) s_ur = diff;
              else if (diff < 0 && diff > s_lr) s_lr = diff;            
            }
            // **[3]** left lane
            else if (d < (4*lane+2)-2) {
              if      (diff > 0 && diff < s_ul) s_ul = diff;
              else if (diff < 0 && diff > s_ll) s_ll = diff;
            } 
          }
          cout << "----------------------------" << endl;
          cout << "|" << s_ul << "|" << "0000000" << "|" << s_ur << "|" << endl;
          cout << "|" << s_ll << "|" << "0000000" << "|" << s_lr << "|" << endl;
 
         
           
          // =================================
          // the FSM logic (setting car_state)
          // =================================         
          double safe_gap = 20.0;

          double gap_l = s_ul - s_ll;
          double gap_r = s_ur - s_lr;
          bool changing = fabs(4*lane+2 - car_d) > 1.0;  
          cout << "changing: " << changing << endl;
          bool l_ok = lane > 0 && gap_l >= safe_gap && s_ul >= 12 && s_ll <= -5;
          bool r_ok = lane < 2 && gap_r >= safe_gap && s_ur >= 12 && s_lr <= -5; 
          if (changing) car_state = KL;
          else {
            if      (l_ok &&  r_ok) 
              if (gap_l > gap_r) car_state = LCL;
              else               car_state = LCR; 
             
            else if (l_ok && !r_ok) car_state = LCL;
            else if (r_ok && !l_ok) car_state = LCR; 
            else                    car_state = KL; 
          }

          // ==================
          // realize any state 
          // ================== 
          if (too_close && ref_vel > v_u) {
            ref_vel -= 0.224;
            if      (LCL == car_state) lane--; 
            else if (LCR == car_state) lane++;
          }
          else if (ref_vel < target_vel) { 
            ref_vel += 0.224;
          }         

          
          // ==========================
          // building a planning spline
          // ==========================

          // points to build spline
          vector<double> ptsx;
          vector<double> ptsy;


          // reference x, y and yaw states
          double ref_x = car_x;
          double ref_y = car_y;
          double ref_yaw = deg2rad(car_yaw);

          vector<double> sd2xy = getXY(pred_s, car_d, map_waypoints_s, map_waypoints_x, map_waypoints_y);
          // cout << "according to simulators s & d: " << '|' << sd2xy[0] << ',' << sd2xy[1] << '|' << endl;
          // cout << "directly, " << '|' << car_x << ',' << car_y << '|' << endl;
           
          // if nothing to refer, use the car as starting reference
          if (pre_size < 2) {
            double pre_car_x = car_x - cos(ref_yaw);
            double pre_car_y = car_y - sin(ref_yaw);

            ptsx.push_back(pre_car_x);
            ptsy.push_back(pre_car_y);
            ptsx.push_back(car_x);
            ptsy.push_back(car_y);

          } 
          // if sufficient to refer the previous 
          else {
            /* TEST track */
            // if (pre_size > 10) { 
            //   cout << "--------------------" << endl;             
            //   cout << "previous path with " << pre_size << " points" << endl;
            //   cout << "the first five:" << endl;
            //   for (int i = pre_size - 5; i < pre_size; ++i) cout << '{' << previous_path_x[i] << ", " << previous_path_y[i] << '}' << endl;
            //   cout << "the last five:" << endl;
            //   for (int i = 0; i < 5; ++i) cout << '{' << previous_path_x[i] << ", " << previous_path_y[i] << '}' << endl;
            // }
            /* END track test */

            ref_x = previous_path_x[pre_size - 1];
            ref_y = previous_path_y[pre_size - 1]; 

            double ref_x_pre = previous_path_x[pre_size - 2];
            double ref_y_pre = previous_path_y[pre_size - 2];
            ref_yaw = atan2(ref_y - ref_y_pre, ref_x - ref_x_pre);

            ptsx.push_back(ref_x_pre);
            ptsy.push_back(ref_y_pre);

            ptsx.push_back(ref_x);
            ptsy.push_back(ref_y);
          }

          /* TEST track */
          // cout << "--------------------" << endl;
          // cout << "============|" << "reference xy : " << ref_x << ", " << ref_y << endl;
          // cout << "============|" << "reference yaw: " << ref_yaw << endl;
          /* END track test */

          // append more points for building spline
          for (int i = 1; i <= 3; ++i) {
            vector<double> wp = getXY(pred_s+i*30, 2+4*lane, map_waypoints_s, map_waypoints_x, map_waypoints_y);
            ptsx.push_back(wp[0]);
            ptsy.push_back(wp[1]); 
          }
          //
          // END of the pts building
          // 


          // for (int i = 0; i < ptsx.size(); ++i) cout << '[' << ptsx[i] << ',' << ptsy[i] << ']' << endl;
            
          // transformation from global to ref's local
          for (int i = 0; i < ptsx.size(); ++i) {
            double rel_x = ptsx[i] - ref_x;
            double rel_y = ptsy[i] - ref_y;
            ptsx[i] = rel_x*cos(ref_yaw) + rel_y*sin(ref_yaw);
            ptsy[i] = rel_y*cos(ref_yaw) - rel_x*sin(ref_yaw);
          }
          // for (int i = 0; i < ptsx.size(); ++i) cout << '(' << ptsx[i] << ',' << ptsy[i] << ')' << endl;

          tk::spline s;
          s.set_points(ptsx, ptsy);

          // ================================
          // sample waypoints from the spline
          // ================================
          vector<double> next_x_vals;
          vector<double> next_y_vals;

          for (int i = 0; i < pre_size; ++i) {
            next_x_vals.push_back(previous_path_x[i]);
            next_y_vals.push_back(previous_path_y[i]);
          }

          double des_x = 30.0;
          double des_y = s(des_x);
          double des_dist = sqrt(des_x*des_x + des_y*des_y);
          double x_inc = des_x / (des_dist/(0.02*ref_vel/2.24));
          for (int i = 0; i < 30 - pre_size; ++i) {
            double x_val = (i+1) * x_inc;
            double y_val = s(x_val);
            double next_x_val = x_val*cos(ref_yaw) - y_val*sin(ref_yaw) + ref_x;
            double next_y_val = x_val*sin(ref_yaw) + y_val*cos(ref_yaw) + ref_y;
            next_x_vals.push_back(next_x_val);
            next_y_vals.push_back(next_y_val); 
          }   

          /* TEST track */
          // for (int i = next_x_vals.size() - 5; i < next_x_vals.size(); ++i) cout << '<' << next_x_vals[i] << ", " << next_y_vals[i] << '>' << endl;
          /* END track test */

          // END TODO


          json msgJson;
          msgJson["next_x"] = next_x_vals;
          msgJson["next_y"] = next_y_vals;

          auto msg = "42[\"control\","+ msgJson.dump()+"]";

          //this_thread::sleep_for(chrono::milliseconds(1000));
          ws.send(msg.data(), msg.length(), uWS::OpCode::TEXT);
          
        }
      } else {
        // Manual driving
        std::string msg = "42[\"manual\",{}]";
        ws.send(msg.data(), msg.length(), uWS::OpCode::TEXT);
      }
    }
  });

  // We don't need this since we're not using HTTP but if it's removed the
  // program
  // doesn't compile :-(
  h.onHttpRequest([](uWS::HttpResponse *res, uWS::HttpRequest req, char *data,
                     size_t, size_t) {
    const std::string s = "<h1>Hello world!</h1>";
    if (req.getUrl().valueLength == 1) {
      res->end(s.data(), s.length());
    } else {
      // i guess this should be done more gracefully?
      res->end(nullptr, 0);
    }
  });

  h.onConnection([&h](uWS::WebSocket<uWS::SERVER> ws, uWS::HttpRequest req) {
    std::cout << "Connected!!!" << std::endl;
  });

  h.onDisconnection([&h](uWS::WebSocket<uWS::SERVER> ws, int code,
                         char *message, size_t length) {
    ws.close();
    std::cout << "Disconnected" << std::endl;
  });

  int port = 4567;
  if (h.listen(port)) {
    std::cout << "Listening to port " << port << std::endl;
  } else {
    std::cerr << "Failed to listen to port" << std::endl;
    return -1;
  }
  h.run();
}
