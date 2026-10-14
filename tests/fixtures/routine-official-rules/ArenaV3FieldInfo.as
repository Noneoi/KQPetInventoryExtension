package mmo.arenav3.model
{
   import mmo.arenav3.ArenaV3DataManager;
   import mmo.arenav3.config.ArenaV3BattleFieldConfig;
   import mmo.arenav3.config.ArenaV3Config;
   import mmo.arenav3.defines.ArenaV3BattleFieldDefine;
   import mmo.arenav3.defines.ArenaV3OncePrizeDefine;
   import mmo.common.CDUtil;
   
   public class ArenaV3FieldInfo
   {
      
      private var _fieldId:int;
      
      private var _curZoneId:int;
      
      private var _hasSetFormation:Boolean;
      
      private var _seasonTopRank:int;
      
      private var _weeklyTopRank:int;
      
      private var _myChallengeTime:int;
      
      private var _remainChallengeTime:int;
      
      private var _canBuyTimeNum:int;
      
      private var _nextChallengeTime:Number;
      
      private var _curSpaceBuffId:int = -1;
      
      private var _oncePrizeStates:Array = [];
      
      private var _hadGotWeeklyPrize:Boolean;
      
      private var _canGainweeklyPrize:Boolean;
      
      private var _changeZoneTimes:int;
      
      private var _remainRefreshTime:int;
      
      private var _zoneInfos:Vector.<ArenaV3ZoneInfo>;
      
      public function ArenaV3FieldInfo()
      {
         super();
      }
      
      public static function parseInfo(param1:int, param2:Object) : ArenaV3FieldInfo
      {
         var _loc6_:ArenaV3OncePrizeDefine = null;
         var _loc3_:ArenaV3FieldInfo = new ArenaV3FieldInfo();
         _loc3_._fieldId = param1;
         _loc3_._curZoneId = param2["curz"];
         if(_loc3_._curZoneId <= 0)
         {
            return _loc3_;
         }
         _loc3_._hasSetFormation = param2["hsf"];
         _loc3_._seasonTopRank = param2["tp"];
         _loc3_._weeklyTopRank = param2["wcf"];
         var _loc4_:Array = [];
         var _loc5_:String = param2["ppz"];
         var _loc7_:int = ArenaV3BattleFieldConfig.getFieldDefineById(_loc3_._fieldId).totalOncePrizeNum;
         var _loc8_:int = 0;
         while(_loc8_ < _loc7_)
         {
            _loc6_ = ArenaV3BattleFieldConfig.getFieldDefineById(_loc3_._fieldId).getOncePrizeDefineByIdx(_loc8_);
            _loc4_[_loc8_] = _loc5_.indexOf(_loc6_.serverZoneId.toString()) > -1;
            _loc8_++;
         }
         _loc3_._oncePrizeStates = _loc4_;
         _loc3_._canBuyTimeNum = Math.max(0,_loc3_.fieldDefine.dailyMaxBuyChallengeTime - param2["bct"]);
         _loc3_._remainRefreshTime = Math.max(0,_loc3_.fieldDefine.dailyRefreshTime - param2["rh"]);
         _loc3_._changeZoneTimes = param2["mz"];
         _loc3_._myChallengeTime = param2["ct"];
         _loc3_._remainChallengeTime = Math.max(0,_loc3_.fieldDefine.dailyFreeChallengeTime - param2["ct"] + param2["bct"]);
         _loc3_._nextChallengeTime = param2["cd"];
         ArenaV3DataManager.instance.parseChallengeInfo(_loc3_._remainChallengeTime,_loc3_._nextChallengeTime);
         var _loc9_:Object = param2;
         _loc9_["fi"] = param1;
         _loc3_._zoneInfos = ArenaV3ZoneInfo.parseInfoList([_loc9_]);
         return _loc3_;
      }
      
      public function get seasonTopRank() : int
      {
         return this._seasonTopRank;
      }
      
      public function get weeklyTopRank() : int
      {
         return this._weeklyTopRank;
      }
      
      public function get myChallengeTime() : int
      {
         return this._myChallengeTime;
      }
      
      public function get remainChallengeTime() : int
      {
         return this._remainChallengeTime;
      }
      
      public function get hasChallengeTime() : Boolean
      {
         return this._remainChallengeTime > 0;
      }
      
      public function get canBuyTimeNum() : int
      {
         return this._canBuyTimeNum;
      }
      
      public function get hasBuyTime() : Boolean
      {
         return this._canBuyTimeNum > 0;
      }
      
      public function get isInCd() : Boolean
      {
         return CDUtil.isInCd(this._nextChallengeTime);
      }
      
      public function get cdToSecTime() : int
      {
         return CDUtil.cdToSecTime(this._nextChallengeTime);
      }
      
      public function get zoneInfos() : Vector.<ArenaV3ZoneInfo>
      {
         return this._zoneInfos;
      }
      
      public function get curZoneInfo() : ArenaV3ZoneInfo
      {
         var tmpInfos:Vector.<ArenaV3ZoneInfo>;
         if(!this._zoneInfos)
         {
            return null;
         }
         tmpInfos = this._zoneInfos.filter(function(param1:ArenaV3ZoneInfo, param2:int, param3:*):*
         {
            return param1.serverZoneId == _curZoneId;
         });
         return tmpInfos.length == 0 ? null : tmpInfos[0];
      }
      
      public function get fieldId() : int
      {
         return this._fieldId;
      }
      
      public function get nextChallengeTime() : Number
      {
         return this._nextChallengeTime;
      }
      
      public function get hasSetFormation() : Boolean
      {
         return this._hasSetFormation;
      }
      
      public function setDefendFormation() : void
      {
         this._hasSetFormation = true;
      }
      
      public function get fieldDefine() : ArenaV3BattleFieldDefine
      {
         return ArenaV3BattleFieldConfig.getFieldDefineById(this._fieldId);
      }
      
      public function get oncePrizeStates() : Array
      {
         return this._oncePrizeStates;
      }
      
      public function get hadAllGotOncePrize() : Boolean
      {
         return this._oncePrizeStates.every(function(param1:Boolean, param2:int, param3:*):*
         {
            return param1 == true;
         });
      }
      
      public function getCanGainOncePrize(param1:int) : Boolean
      {
         var _loc3_:int = 0;
         var _loc2_:ArenaV3OncePrizeDefine = this.fieldDefine.getOncePrizeDefineByRank(this._seasonTopRank);
         if(_loc2_ == null)
         {
            return false;
         }
         _loc3_ = _loc2_.idx;
         return param1 >= _loc3_;
      }
      
      public function get hadOncePrizeCanGainButNotGain() : Boolean
      {
         var _loc2_:int = 0;
         var _loc3_:int = 0;
         var _loc1_:ArenaV3OncePrizeDefine = this.fieldDefine.getOncePrizeDefineByRank(this._seasonTopRank);
         if(_loc1_ == null)
         {
            return false;
         }
         _loc2_ = _loc1_.idx;
         _loc3_ = _loc2_;
         while(_loc3_ < this.fieldDefine.totalOncePrizeNum)
         {
            if(!this._oncePrizeStates[_loc3_])
            {
               return true;
            }
            _loc3_++;
         }
         return false;
      }
      
      public function get hasDailyRefreshTime() : Boolean
      {
         return this.remainRefreshTime > 0;
      }
      
      public function get isFirstChangeZone() : Boolean
      {
         return this._changeZoneTimes <= 0;
      }
      
      public function get curZoneId() : int
      {
         return this._curZoneId;
      }
      
      public function get changeZoneTimes() : int
      {
         return this._changeZoneTimes;
      }
      
      public function get remainRefreshTime() : int
      {
         return this._remainRefreshTime;
      }
      
      public function get changeZoneNeedDiamond() : int
      {
         return ArenaV3Config.getChangeZoneNeedDiamondNum(this._changeZoneTimes);
      }
      
      public function get canGainweeklyPrize() : Boolean
      {
         return this._canGainweeklyPrize;
      }
      
      public function get hadGotWeeklyPrize() : Boolean
      {
         return this._hadGotWeeklyPrize;
      }
      
      public function afterTakeWeeklyBonus() : void
      {
         this._hadGotWeeklyPrize = true;
      }
      
      public function afterBuyChallengeTime(param1:int) : void
      {
         this._remainChallengeTime += param1;
         this._canBuyTimeNum -= param1;
         this.afterClearCd();
      }
      
      public function afterClearCd() : void
      {
         this._nextChallengeTime = 0;
      }
      
      public function afterGainOncePrize(param1:int) : void
      {
         this._oncePrizeStates[param1] = true;
      }
      
      public function afterRefresh() : void
      {
         --this._remainRefreshTime;
      }
      
      public function afterSweep(param1:Object) : void
      {
         ++this._myChallengeTime;
         --this._remainChallengeTime;
         this._weeklyTopRank = param1["wcf"];
      }
   }
}

