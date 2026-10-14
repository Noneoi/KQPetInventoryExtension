package mmo.arenav3.defines
{
   import mmo.common.DateUtil;
   
   public class ArenaV3BattleFieldDefine
   {
      
      private static const SPECIAL_CONSTRUCTOR:Object = {
         "zoneDefines":ArenaV3ZoneDefine.createWithObjArr,
         "reachRankOncePrizes":ArenaV3OncePrizeDefine.createWithObjArr,
         "dailyCloseTime":praseTime,
         "weeklySettleCloseTime":praseTime,
         "seasonOpenTime":DateUtil.parse
      };
      
      private static var isLock:Boolean = false;
      
      private var _id:int;
      
      private var _zoneDefines:Vector.<ArenaV3ZoneDefine>;
      
      private var _reachRankOncePrizes:Vector.<ArenaV3OncePrizeDefine>;
      
      private var _dailyFreeChallengeTime:int;
      
      private var _dailyMaxBuyChallengeTime:int;
      
      private var _buyChallengeTimePrice:int;
      
      private var _dailyRefreshTime:int;
      
      private var _dailySweepTime:int;
      
      private var _enterMinBattlePower:int;
      
      private var _clearCdPrice:int;
      
      private var _seasonId:int;
      
      private var _seasonOpenTime:Date;
      
      private var _dailyCloseTime:Date;
      
      private var _weeklySettleCloseTime:Date;
      
      private var _defendActName:String;
      
      private var _desc:String;
      
      private var _rule:String;
      
      public function ArenaV3BattleFieldDefine()
      {
         super();
         if(!isLock)
         {
            throw new Error("Can\'t not construct outer!");
         }
      }
      
      private static function praseTime(param1:String) : Date
      {
         var _loc2_:Date = DateUtil.getServerTime();
         var _loc3_:Array = param1.split(":");
         return new Date(_loc2_.fullYear,_loc2_.month,_loc2_.date,_loc3_[0],_loc3_[1],_loc3_[2]);
      }
      
      public static function createWithObjArr(param1:Array) : Vector.<ArenaV3BattleFieldDefine>
      {
         var fieldDefinesVec:Vector.<ArenaV3BattleFieldDefine> = null;
         var tmpObjArr:Array = param1;
         fieldDefinesVec = new Vector.<ArenaV3BattleFieldDefine>();
         tmpObjArr.forEach(function(param1:Object, param2:int, param3:Array):*
         {
            fieldDefinesVec[param2] = ArenaV3BattleFieldDefine.createWithObj(param1);
         });
         return fieldDefinesVec;
      }
      
      private static function createWithObj(param1:Object) : ArenaV3BattleFieldDefine
      {
         var _loc3_:String = null;
         isLock = true;
         var _loc2_:ArenaV3BattleFieldDefine = new ArenaV3BattleFieldDefine();
         for(_loc3_ in param1)
         {
            if(SPECIAL_CONSTRUCTOR[_loc3_] != null)
            {
               _loc2_["_" + _loc3_] = SPECIAL_CONSTRUCTOR[_loc3_](param1[_loc3_]);
            }
            else
            {
               _loc2_["_" + _loc3_] = param1[_loc3_];
            }
         }
         isLock = false;
         return _loc2_;
      }
      
      public function get zoneDefines() : Vector.<ArenaV3ZoneDefine>
      {
         return this._zoneDefines;
      }
      
      public function get totalZoneNum() : int
      {
         return this._zoneDefines.length;
      }
      
      public function getZoneDefineByIdx(param1:int) : ArenaV3ZoneDefine
      {
         var idx:int = param1;
         var tmpDefines:Vector.<ArenaV3ZoneDefine> = this._zoneDefines.filter(function(param1:ArenaV3ZoneDefine, param2:int, param3:*):*
         {
            return param1.idx == idx;
         });
         return tmpDefines.length == 0 ? null : tmpDefines[0];
      }
      
      public function getZoneDefineByServerId(param1:int) : ArenaV3ZoneDefine
      {
         var serverZoneId:int = param1;
         var tmpDefines:Vector.<ArenaV3ZoneDefine> = this._zoneDefines.filter(function(param1:ArenaV3ZoneDefine, param2:int, param3:*):*
         {
            return param1.serverZoneId == serverZoneId;
         });
         return tmpDefines.length == 0 ? null : tmpDefines[0];
      }
      
      public function get reachRankOncePrizes() : Vector.<ArenaV3OncePrizeDefine>
      {
         return this._reachRankOncePrizes;
      }
      
      public function getOncePrizeDefineByIdx(param1:int) : ArenaV3OncePrizeDefine
      {
         var idx:int = param1;
         var tmpDefines:Vector.<ArenaV3OncePrizeDefine> = this._reachRankOncePrizes.filter(function(param1:ArenaV3OncePrizeDefine, param2:int, param3:*):*
         {
            return param1.idx == idx;
         });
         return tmpDefines.length == 0 ? null : tmpDefines[0];
      }
      
      public function get totalOncePrizeNum() : int
      {
         return this._reachRankOncePrizes.length;
      }
      
      public function getOncePrizeDefineByRank(param1:int) : ArenaV3OncePrizeDefine
      {
         var rank:int = param1;
         var tmpDefines:Vector.<ArenaV3OncePrizeDefine> = this._reachRankOncePrizes.filter(function(param1:ArenaV3OncePrizeDefine, param2:int, param3:*):*
         {
            return param1.checkIsInRankRange(rank);
         });
         return tmpDefines.length == 0 ? null : tmpDefines[0];
      }
      
      public function get desc() : String
      {
         return this._desc;
      }
      
      public function get rule() : String
      {
         return this._rule;
      }
      
      public function get dailyFreeChallengeTime() : int
      {
         return this._dailyFreeChallengeTime;
      }
      
      public function get dailyMaxBuyChallengeTime() : int
      {
         return this._dailyMaxBuyChallengeTime;
      }
      
      public function get buyChallengeTimePrice() : int
      {
         return this._buyChallengeTimePrice;
      }
      
      public function get clearCdPrice() : int
      {
         return this._clearCdPrice;
      }
      
      public function get id() : int
      {
         return this._id;
      }
      
      public function get dailyRefreshTime() : int
      {
         return this._dailyRefreshTime;
      }
      
      public function get dailySweepTime() : int
      {
         return this._dailySweepTime;
      }
      
      public function get enterMinBattlePower() : int
      {
         return this._enterMinBattlePower;
      }
      
      public function get seasonId() : int
      {
         return this._seasonId;
      }
      
      public function get weeklySettleCloseTime() : Date
      {
         return this._weeklySettleCloseTime;
      }
      
      public function get dailyCloseTime() : Date
      {
         return this._dailyCloseTime;
      }
      
      public function get isArenaClose() : Boolean
      {
         return this.isArenaWeeklyUpdateClose || DateUtil.getServerTime().time >= this._dailyCloseTime.time;
      }
      
      public function get isArenaWeeklyUpdateClose() : Boolean
      {
         return DateUtil.getServerTime().day == 4 && DateUtil.getServerTime().time >= this._weeklySettleCloseTime.time;
      }
      
      public function get seasonOpenTime() : Date
      {
         return this._seasonOpenTime;
      }
      
      public function get defendActName() : String
      {
         return this._defendActName;
      }
      
      public function get isInSeasonOpenTime() : Boolean
      {
         var _loc1_:Date = DateUtil.getServerTime();
         if(_loc1_.hours < 5)
         {
            return true;
         }
         return _loc1_.time > this._seasonOpenTime.time;
      }
      
      public function get seasonOpenWaitSeconds() : int
      {
         var _loc1_:Date = null;
         if(this.isInSeasonOpenTime)
         {
            return 0;
         }
         _loc1_ = DateUtil.getServerTime();
         return (this._seasonOpenTime.time - _loc1_.time) / 1000;
      }
   }
}

