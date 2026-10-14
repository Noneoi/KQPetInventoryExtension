package mmo.activityext.newact20260522.farmv2.model
{
   import mmo.common.helper.ActUtil;
   import mmo.common.utils.dt.ClientXT;
   import mmo.loader.common.datastructures.HashMap;
   
   public class FV2Client extends ClientXT
   {
      
      public static var MOCK:Boolean = false;
      
      public static const CMD_GET_INFO:String = "1008_20260522_nf_0";
      
      public static const CMD_REFRESH_FARM:String = "1008_20260522_nf_1";
      
      public static const CMD_ENTER_FARM:String = "1008_20260522_nf_2";
      
      public static const CMD_PICK:String = "1008_20260522_nf_3";
      
      public static const CMD_GET_BOX:String = "1008_20260522_nf_4";
      
      public static const CMD_BUY:String = "1008_20260522_nf_5";
      
      public static const CMD_ACCEPT_INVITE:String = "1008_20260522_nf_6";
      
      public static const CMD_CONVERT:String = "1008_20260522_nf_7";
      
      public static const CMD_GET_FLOWER:String = "1008_20260522_nf_8";
      
      private static const CMD_PICK_ONEKEY:String = "1008_20260522_nf_9";
      
      private static const ERR_CODE_ERR_MSG:HashMap = new HashMap();
      
      ERR_CODE_ERR_MSG.add(-1,"还处于冷却时间当中");
      ERR_CODE_ERR_MSG.add(-2,"农场已被使用");
      ERR_CODE_ERR_MSG.add(-3,"刷新农场扣除失败");
      ERR_CODE_ERR_MSG.add(-4,"采摘次数不足");
      ERR_CODE_ERR_MSG.add(-5,"农场不存在");
      ERR_CODE_ERR_MSG.add(-6,"农田已被采摘");
      ERR_CODE_ERR_MSG.add(-7,"获取奖励错误");
      ERR_CODE_ERR_MSG.add(-8,"获取宝箱错误");
      ERR_CODE_ERR_MSG.add(-9,"购买错误");
      ERR_CODE_ERR_MSG.add(-10,"邀请人数达到上限");
      ERR_CODE_ERR_MSG.add(-11,"转换物品不存在");
      ERR_CODE_ERR_MSG.add(-12,"转换物品失败");
      ERR_CODE_ERR_MSG.add(-13,"不能邀请自己");
      ERR_CODE_ERR_MSG.add(-14,"购买达到上限");
      ERR_CODE_ERR_MSG.add(-15,"不能重复邀请");
      
      public function FV2Client()
      {
         super(null,ERR_CODE_ERR_MSG);
      }
      
      public function getInfo(param1:Function, param2:int = 0) : void
      {
         var callback:Function = param1;
         var un:int = param2;
         if(MOCK)
         {
            ActUtil.applyCallback(callback,[this.mockGetInfo()]);
            return;
         }
         request(CMD_GET_INFO,function(param1:Object):void
         {
            var _loc2_:Array = null;
            var _loc3_:Array = null;
            var _loc4_:int = 0;
            var _loc5_:String = null;
            if(param1["rft"] != null)
            {
               param1["rt"] = param1["rft"];
            }
            if(param1["fds"] != null)
            {
               _loc2_ = param1["fds"];
               _loc3_ = [];
               _loc4_ = 0;
               while(_loc4_ < _loc2_.length)
               {
                  _loc3_.push({
                     "level":_loc2_[_loc4_],
                     "picked":Boolean(param1["fus"][_loc4_])
                  });
                  _loc4_++;
               }
               param1["farms"] = _loc3_;
            }
            if(param1["in"] != null)
            {
               _loc5_ = String(param1["in"]);
               param1["invites"] = _parseInviteString(_loc5_);
            }
            else
            {
               param1["invites"] = [];
            }
            if(param1["wpt"] != null)
            {
               param1["wbp"] = param1["wpt"];
            }
            ActUtil.applyCallback(callback,[param1]);
         },{"un":un});
      }
      
      private function _parseInviteString(param1:String) : Array
      {
         var _loc4_:String = null;
         var _loc5_:Array = null;
         var _loc6_:String = null;
         var _loc7_:Number = NaN;
         var _loc8_:Array = null;
         var _loc9_:int = 0;
         var _loc10_:String = null;
         var _loc11_:int = 0;
         var _loc2_:Array = [];
         if(!param1 || param1.length == 0)
         {
            return _loc2_;
         }
         var _loc3_:Array = param1.split("#");
         for each(_loc4_ in _loc3_)
         {
            if(!(!_loc4_ || _loc4_.length == 0 || _loc4_ == "-1"))
            {
               _loc5_ = _loc4_.split(":");
               if(_loc5_.length >= 2)
               {
                  _loc6_ = _loc5_[0];
                  _loc7_ = Number(_loc5_[1]);
                  _loc8_ = _loc6_.split("|");
                  if(_loc8_.length >= 2)
                  {
                     _loc9_ = int(_loc8_[0]);
                     _loc10_ = _loc8_[1];
                     if(_loc9_ > 0)
                     {
                        _loc11_ = Math.max(0,Math.ceil(_loc7_ / 86400));
                        _loc2_.push({
                           "uid":_loc9_,
                           "name":_loc10_,
                           "expireDay":_loc11_,
                           "clothes":""
                        });
                     }
                  }
               }
            }
         }
         return _loc2_;
      }
      
      public function refresh(param1:Function) : void
      {
         var callback:Function = param1;
         if(MOCK)
         {
            ActUtil.applyCallback(callback,[this.mockRefresh()]);
            return;
         }
         request(CMD_REFRESH_FARM,function(param1:Object):void
         {
            var _loc2_:Array = null;
            var _loc3_:Array = null;
            var _loc4_:int = 0;
            if(param1["fds"] != null)
            {
               _loc2_ = param1["fds"];
               _loc3_ = [];
               _loc4_ = 0;
               while(_loc4_ < _loc2_.length)
               {
                  _loc3_.push({
                     "level":_loc2_[_loc4_],
                     "picked":false
                  });
                  _loc4_++;
               }
               param1["farms"] = _loc3_;
            }
            ActUtil.applyCallback(callback,[param1]);
         });
      }
      
      public function enterFarm(param1:int, param2:Function) : void
      {
         var farmIndex:int = param1;
         var callback:Function = param2;
         if(MOCK)
         {
            ActUtil.applyCallback(callback,[this.mockEnterFarm()]);
            return;
         }
         request(CMD_ENTER_FARM,function(param1:Object):void
         {
            var _loc2_:Array = param1["fls"] || [];
            var _loc3_:Array = [];
            var _loc4_:int = 0;
            while(_loc4_ < _loc2_.length)
            {
               _loc3_.push({
                  "index":_loc4_,
                  "cropId":_loc2_[_loc4_]
               });
               _loc4_++;
            }
            param1["fields"] = _loc3_;
            ActUtil.applyCallback(callback,[param1]);
         },{"fi":farmIndex});
      }
      
      public function pickField(param1:int, param2:int, param3:Function) : void
      {
         if(MOCK)
         {
            ActUtil.applyCallback(param3,[this.mockPickField(param2)]);
            return;
         }
         request(CMD_PICK,param3,{"fi":param2});
      }
      
      public function pickFieldOneKey(param1:Function) : void
      {
         request(CMD_PICK_ONEKEY,param1,null);
      }
      
      public function openChest(param1:int, param2:int, param3:Function) : void
      {
         if(MOCK)
         {
            ActUtil.applyCallback(param3,[this.mockOpenChest()]);
            return;
         }
         request(CMD_GET_BOX,param3,{"ci":param2});
      }
      
      public function buyPickTimes(param1:int, param2:Function) : void
      {
         if(MOCK)
         {
            ActUtil.applyCallback(param2,[{
               "pt":3,
               "ptMax":6,
               "wbp":4
            }]);
            return;
         }
         request(CMD_BUY,param2,{
            "bi":1,
            "n":1
         });
      }
      
      public function convertItems(param1:Array, param2:int, param3:Function) : void
      {
         if(MOCK)
         {
            ActUtil.applyCallback(param3,[{
               "shard":10,
               "convertable":[]
            }]);
            return;
         }
         var _loc4_:String = param1.join("#");
         request(CMD_CONVERT,param3,{
            "m":_loc4_,
            "ci":param2
         });
      }
      
      public function acceptInvite(param1:int, param2:Function, param3:Function = null) : void
      {
         if(MOCK)
         {
            ActUtil.applyCallback(param2,[{}]);
            return;
         }
         request(CMD_ACCEPT_INVITE,param2,{"uid":param1},true,param3);
      }
      
      public function getFlower(param1:int, param2:Function) : void
      {
         if(MOCK)
         {
            ActUtil.applyCallback(param2);
            return;
         }
         request(CMD_GET_FLOWER,param2,{"ci":param1});
      }
      
      private function mockGetInfo() : Object
      {
         return {
            "rt":3,
            "rtMax":10,
            "pt":2,
            "ptMax":6,
            "shard":5,
            "wbp":3,
            "wbr":0,
            "farms":[{
               "level":1,
               "picked":false
            },{
               "level":2,
               "picked":true
            },{
               "level":3,
               "picked":false
            },{
               "level":1,
               "picked":false
            },{
               "level":4,
               "picked":false
            }],
            "invites":[{
               "uid":1001,
               "name":"测试玩家A",
               "expireDay":20,
               "clothes":"1@2;6;7;8;10;11&3216"
            },{
               "uid":1002,
               "name":"测试玩家B",
               "expireDay":5,
               "clothes":"1@2;6;7;8;10;11&3216;3220;3212;3213;3214;3215"
            }],
            "inviteCount":2,
            "inviteLink":"https://example.com/invite?code=MOCK123",
            "convertable":[{
               "itemId":"8:1:100",
               "count":3,
               "shardOut":3
            },{
               "itemId":"8:1:101",
               "count":1,
               "shardOut":5
            }]
         };
      }
      
      private function mockRefresh() : Object
      {
         return {
            "rt":2,
            "shard":0,
            "farms":[{
               "level":2,
               "picked":false
            },{
               "level":1,
               "picked":false
            },{
               "level":4,
               "picked":false
            },{
               "level":3,
               "picked":false
            },{
               "level":1,
               "picked":false
            }]
         };
      }
      
      private function mockEnterFarm() : Object
      {
         var _loc3_:int = 0;
         var _loc4_:int = 0;
         var _loc5_:Array = null;
         var _loc6_:Array = null;
         var _loc7_:Array = null;
         var _loc8_:Array = null;
         var _loc9_:Array = null;
         var _loc10_:Array = null;
         var _loc1_:Array = [];
         var _loc2_:int = 0;
         while(_loc2_ < 9)
         {
            _loc3_ = Math.min(Math.floor(_loc2_ / 2),5);
            if(_loc3_ == 0)
            {
               _loc5_ = [5,6,15];
               _loc4_ = int(_loc5_[int(Math.random() * _loc5_.length)]);
            }
            else if(_loc3_ == 1)
            {
               _loc6_ = [1,2,13];
               _loc4_ = int(_loc6_[int(Math.random() * _loc6_.length)]);
            }
            else if(_loc3_ == 2)
            {
               _loc7_ = [3,4,14];
               _loc4_ = int(_loc7_[int(Math.random() * _loc7_.length)]);
            }
            else if(_loc3_ == 3)
            {
               _loc8_ = [7,8,16];
               _loc4_ = int(_loc8_[int(Math.random() * _loc8_.length)]);
            }
            else if(_loc3_ == 4)
            {
               _loc9_ = [9,10,17];
               _loc4_ = int(_loc9_[int(Math.random() * _loc9_.length)]);
            }
            else
            {
               _loc10_ = [11,12,18];
               _loc4_ = int(_loc10_[int(Math.random() * _loc10_.length)]);
            }
            _loc1_.push({
               "index":_loc2_,
               "cropId":String(_loc2_ + 1 + 9)
            });
            _loc2_++;
         }
         return {
            "fields":_loc1_,
            "box":[5,5,5]
         };
      }
      
      private function mockPickField(param1:int) : Object
      {
         return {
            "index":param1,
            "r":0
         };
      }
      
      private function mockOpenChest() : Object
      {
         return {"rewards":[{
            "itemId":"138:90:1",
            "count":1
         },{
            "itemId":"8:1:1",
            "count":2
         }]};
      }
   }
}

